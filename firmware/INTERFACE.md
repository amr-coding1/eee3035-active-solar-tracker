# Interface between the control system and the data link (contract 7.7)

The data link owns: the three INA226 power sensors, the DS3231 clock, the flash
buffer, Wi-Fi and the upload. The control system owns: sun-sensor arc, panel
angle, motor, limit switches. The control system tells the data link what it
is doing through a handful of functions; nothing else is shared.

```cpp
#include "src/core/ast_telemetry.h"   // from the sketch folder

namespace ast {
enum class State : uint8_t { TRK, MOV, HLD, IDL, STW, FLT };

void telemetry_set_angles(float panel_deg, float sun_deg, bool sun_valid);
void telemetry_set_state(State state);
void telemetry_set_motor_running(bool running);
void telemetry_set_fault(uint8_t code);
void telemetry_set_temperature(float deg_c);   // optional (F6)
void telemetry_clear_temperature();
void telemetry_set_run(uint16_t run_id);       // optional, normally the CLI `run`
void telemetry_set_panel(char panel_letter);   // optional, normally the CLI `panel`
}
```

| Function | Call it | Meaning |
|---|---|---|
| `telemetry_set_angles` | every control cycle (at least once per second) | `panel_deg`: panel angle about the tracking axis in degrees, 0 = level, **east negative, west positive**. `sun_deg`: sun angle from the sensor arc on the same axis. `sun_valid = false` when the arc cannot see the sun (dark, cloud, sensor fault): field5 is then omitted. |
| `telemetry_set_state` | every control cycle | `TRK` tracking, `HLD` holding (sensor uncertain), `IDL` idle (dark), `STW` stowed, `FLT` fault. `MOV` need not be sent: the data link reports `MOV` for every minute in which the motor ran. |
| `telemetry_set_motor_running` | `true` when the motor starts, `false` when it stops | Latched: a burst shorter than a second is still seen. |
| `telemetry_set_fault` | every control cycle with the current code | 0 none, 5 sun-sensor arc fault, 7 limit switch hit unexpectedly, 8 motor stall or timeout. A non-zero code wins over the data link's own codes; a code seen at any time in a minute is reported for that minute. |
| `telemetry_set_temperature` | when a panel temperature sensor is fitted | Otherwise field6 is omitted. |

Rules:

- Call from a task or from `loop()`, **not from an interrupt handler** (the
  functions take a mutex for a few microseconds).
- If none of the first four functions is called for more than 5 s, the data
  link treats the control system as silent: angles unknown, state `IDL`.
- Powers and battery voltage are NOT passed in: the data link reads the
  INA226 itself. The power-system lead supplies the shunt values
  (`cal <trk|fix|use> shunt <ohms>`).
- I2C bus: the data link's tasks use `Wire` on A4/A5. Control code that also
  uses I2C must take the bus mutex around each transaction:

  ```cpp
  #include "src/esp_app.h"
  { std::lock_guard<std::mutex> lock(ast_esp::i2c_mutex()); /* Wire.beginTransmission ... */ }
  ```
- CPU frequency is 80 MHz (160 MHz during uploads) and the on-board LEDs are
  switched off to save energy; `delay()`/`millis()` are unaffected.
- Do not call `Serial.begin()` again and do not read from `Serial` (the CLI
  owns the input); `Serial.print` for debugging is fine.

## Minimal example

Control code added to `ast_datalink.ino` (or its own `.cpp` in the sketch):

```cpp
#include "src/core/ast_telemetry.h"
#include "src/esp_app.h"

static void control_task(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(200));          // 5 Hz control loop

    float sun_deg = 0;
    const bool sun_ok = read_sun_arc(&sun_deg);          // your code
    const float panel_deg = read_panel_angle();          // your code
    const uint8_t fault = check_limits_and_stall();      // 0, 5, 7 or 8

    ast::telemetry_set_angles(panel_deg, sun_deg, sun_ok);
    ast::telemetry_set_fault(fault);

    if (fault != 0) {
      motor_stop();
      ast::telemetry_set_motor_running(false);
      ast::telemetry_set_state(ast::State::FLT);
    } else if (!sun_ok) {
      ast::telemetry_set_state(is_dark() ? ast::State::IDL : ast::State::HLD);
    } else if (fabsf(sun_deg - panel_deg) > 2.5f) {
      motor_run_towards(sun_deg);                         // returns immediately
      ast::telemetry_set_motor_running(true);
      ast::telemetry_set_state(ast::State::TRK);
    } else {
      motor_stop();
      ast::telemetry_set_motor_running(false);
      ast::telemetry_set_state(ast::State::TRK);
    }
  }
}

void setup() {
  ast_esp::app_setup(defaults);                           // data link first (starts its tasks)
  xTaskCreatePinnedToCore(control_task, "control", 4096, nullptr, 4, nullptr, 1);
}
```

What ends up in channel 1 for such a minute: `field4` = last `panel_deg`,
`field5` = last `sun_deg` (omitted if `sun_ok` was false at the end of the
minute), status `TRK;r=12;p=A;e=0` or `MOV;...` if the motor ran.

## Without control code

The sketch runs on its own (bench tests of the data link): field4/field5 are
omitted, state `IDL`. `sim on` replaces all inputs by the built-in simulator;
every such sample is marked `s=1`.
