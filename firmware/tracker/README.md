# Tracker firmware (Arduino Nano ESP32)

EEE3035 Group 8, Active Solar Tracker. One program with one file pair per
subsystem, so that each of us can work on our own part:

| File | What it does | Owner |
|---|---|---|
| `main.cpp` | `setup()` and `loop()`: starts the modules, runs the control cycle at 5 Hz | AM (with CO) |
| `config.h` | every pin and setting, in one place | all |
| `transmission.h/.cpp` | data sending: power sensors, clock, flash buffer, Wi-Fi, ThingSpeak uploads, serial commands | AM |
| `sensing.h/.cpp` | photodiode arc: four BPX65 readings to a light direction | CO (starter code by AM) |
| `servo.h/.cpp` | DS-R001 servo: pulse, power switch, move sequence | CH (starter code by AM) |
| `control.h/.cpp` | tracking decisions: deadband, range, stow, faults, manual and test commands | CO (starter code by AM) |
| `src/datalink/` | generated copy of the data link (`firmware/core` + `firmware/ast_datalink/src`). Do not edit it here. | AM |

"Running at the same time" works like this: the data link runs in its own
FreeRTOS tasks (1 Hz sampling, uploads), and `loop()` calls `control::step()`
five times a second and keeps the servo sequence and the serial port going in
between. Nothing blocks for long, so all parts run together.

The decisions (`core/ast_sun`, `core/ast_servo`, `core/ast_tracker`) are plain
C++ and are unit-tested on a PC, including a closed-loop "digital twin" that
moves a simulated lamp across the arc (`firmware/host/tests`). The `.cpp` files
here only connect them to the pins.

**Status, 19 Nov 2026: compiles with no warnings (Arduino IDE / arduino-cli on
esp32 core 3.3.12 and 4.0.0, and PlatformIO) and passes 211 PC tests. It has not run on the real board yet.** Wednesday's bench session
is the first hardware test: follow "First power-up" below.

## Build and flash

Choose one:

1. **Arduino IDE 2.** Boards Manager: install "esp32 by Espressif Systems",
   **version 3.3.12** (tested). Tools > Board > Arduino Nano ESP32; Tools >
   Partition Scheme > **With SPIFFS partition (advanced)**. Open
   `firmware/tracker/tracker.ino`; the other files open as tabs. Upload.
2. **VS Code + PlatformIO.** Open the `firmware/` folder (it holds
   `platformio.ini`). Use the PlatformIO toolbar: Build, Upload, Serial Monitor.
   It uses the community "pioarduino" platform pinned to core 3.3.12, because
   PlatformIO's own ESP32 platform stops at core 2.x and this code needs 3.x.
3. **Command line.** `firmware/build.sh esp32` compiles both sketches;
   `firmware/build.sh upload /dev/ttyACM0` flashes this one (Windows: `COM5`).

Wi-Fi and the ThingSpeak key: either copy `secrets.example.h` to `secrets.h`
(git-ignored) before building, or type `wifi "<hotspot>" <password>` and
`keys 3578277 <WRITE KEY>` in the serial monitor once (stored on the board).
Use a 2.4 GHz phone hotspot; never put university credentials on the board.

Serial monitor: 115200 baud, line ending "Newline". Type `help`.

## Wiring

No Nano ESP32 pin takes 5 V. Everything on the logic side runs from 3V3.

| Signal | Nano pin | Goes to |
|---|---|---|
| Photodiodes, east to west | A0, A1, A2, A3 | BPX65 + load resistor to GND, powered from **3V3** (ADC1: works with Wi-Fi on) |
| I2C SDA / SCL | A4 / A5 | all power sensors and the DS3231, modules on 3V3 |
| Servo signal | D9 | DS-R001 orange wire through 330 Ω |
| Servo power switch | D8 | gate driver of the high-side switch (HIGH = servo on) |
| Panel pot (optional) | D7 | wiper of the 10 kΩ pot on the panel shaft (ends to 3V3 and GND) |
| Board supply | USB-C | power bank (VIN needs 6-21 V; VBUS is not an input) |

Power sensors, as configured in `config.h` (the boot log prints what it finds):

| Address | Measures | Channel field |
|---|---|---|
| 0x40 | tracked panel into its 15 Ω load | field1 P_trk |
| 0x41 | fixed panel into its 15 Ω load | field2 P_fix |
| 0x44 | 5 V feed to the tracker (MCU, sensors, servo) | field3 P_use, field7 V_supply |
| (optional) | second supply branch, added to P_use | `kInaUse2` |

INA226 or INA219 both work: the firmware asks each chip what it is. A
four-channel INA219 board usually answers at 0x40-0x43, so change the
addresses in `config.h` to match the scan. If the MCU and the servo come off
the power bank as two separate branches, measure both and set `kInaUse2`, or
P_use misses one of them.

### Servo power switch

The DS-R001 draws its standby current (10-40 mA, listings disagree) whenever
it is powered. Over a day that is 1.2-4.8 Wh, more than the whole December
tracking gain (about 0.7 Wh/day). So the firmware only powers it for a move:

```
   +5 V (after the consumption sensor)
     |
     +---- 1000 uF + 100 nF to GND   <- bulk capacitor BEFORE the switch: stays charged
     |
     +-------------+
   100 k           | S
     |         P-MOSFET (AO3401 or IRLML6402)
     +-------------| G
     |   100 nF    | D
     +---||--+     +---- 10-47 uF ----> servo red (V+)
         (G-S)     |      to GND, at the servo
               22 k
                   | D
  D8 --1 k-- G  N-MOSFET (2N7002)
           |     | S
         100 k   |
           |     |
          GND   GND

  D9 --330 R--> servo orange (signal)        servo brown --> common GND
```

D8 HIGH turns the N-FET on, which pulls the P-FET gate low through 22 kΩ:
servo powered. The 100 nF from gate to source with the 22 kΩ slows the switch
(a few ms) so it does not hit the supply with a current spike; the big
capacitor sits **before** the switch, so it is already charged and feeds the
servo's start current instead of being charged through the switch (which would
pull the Nano's 5 V down and can reset it). The 100 kΩ pull-down keeps the
servo off while the board boots. Before power goes off the firmware stops the
pulses, waits for the last one to finish (25 ms), then holds D9 LOW, so the
signal pin cannot feed the servo. Without the switch, set `kServoPowerPin = -1`
(the servo then stays powered: fine for a bench test, not for the energy
result).

## First power-up (bench, Wednesday 25 Nov)

**After every boot the servo stays off** until someone types `servo <deg>` or
`servo auto` (`kStartTracking = false` in `config.h`), so nothing moves before
the direction, level and limits have been checked. Do these in order, with
the serial monitor open. Note every result in the lab book (date, time, who).

1. **Boot log.** It lists the I2C devices and which chip answers at each
   address. Fix `config.h` addresses if needed. `status`: time valid? (needs
   the DS3231, or Wi-Fi for NTP).
2. **Power sensors.** `cal` shows raw and calibrated V and mA of each sensor.
   Set the shunt values (`cal use shunt 0.1`) and, if there is time, the
   two-point calibration against the DMM (firmware/README.md).
3. **Photodiodes.** Lamp on, `sun`: four readings, the estimate and why. No
   channel should say SATURATED; if one does, use a smaller load resistor.
   Cover all four, `sun cal dark`; put paper over them under even light,
   `sun cal flat`. Then move the lamp to a few known angles: `sun` also prints
   how well a cos^1 and a cos^2 response fit; if cos^2 fits clearly better,
   set `kSunResponsePower = 2`. "Diffuse light" means no clear beam (hold);
   "readings do not fit" means a dead, shaded or mis-wired photodiode.
4. **Servo, by hand.** Hold the panel clear of anything, then `servo 0`: the
   panel should be level (inclinometer). The very first move after flashing
   goes straight to its target at full speed (the old angle is unknown); later
   boots start from the angle saved after the last clean move. `servo test`
   steps through -45 ... +45 deg, 8 s each: write down commanded vs
   inclinometer. Put the average offset in `kServoTrimDeg`; if the panel turns
   the wrong way, flip `kServoWestIsHigherPulse`. If the board resets when the
   servo starts: capacitor before the switch, slower gate (see the switch
   above), shorter wires, slower `kServoSlewDegPerS`.
5. **Holding test (M9).** `servo off`, then push the panel edge gently
   (about 10 N). Does it hold unpowered? If not, the power switch saves energy
   but lets the panel drift: tell the group (design decision).
6. **Tracking.** `servo auto`, `live on`. Move the lamp slowly: the panel
   follows in steps of about 5 deg. Lamp off: state IDL, then STW after 1 min
   (it parks at -45 deg).
7. **Data.** `upload` sends now; `status` shows the result. The dashboard
   (link in the main README) shows the minute records within about 15 min,
   or press `upload` and refresh.

## Serial commands

Data link (`help` lists all): `status`, `time`, `buffer`, `upload`,
`upload every <5-60>`, `run <id>`, `panel A|B`, `sim on|off`, `cal`, `power`,
`wifi <ssid> <pass>`, `keys <ch1> <write> [read]`, `reboot`.

Tracker:

| Command | Does |
|---|---|
| `sun` | photodiode readings, estimate, calibration |
| `sun cal dark` / `sun cal flat` / `sun cal clear` | store dark offsets / gains / reset |
| `pot` | panel-angle pot reading (if fitted) |
| `servo <deg>` | manual: move to that panel angle and hold |
| `servo test` | manual steps -45 ... +45 deg, 8 s each |
| `servo auto` | start (or go back to) tracking |
| `servo off` | switch the servo off now and hold |
| `live on` / `live off` | one line a second: sun, panel, powers, state |
| `ctrl` / `ctrl reset` | control status / clear a latched servo fault |

## Settings worth tuning on the bench (`config.h`)

| Setting | Default | Why |
|---|---|---|
| `kSunSensorDeg` | -45, -15, 15, 45 | the directions the photodiodes really face |
| `kSunArcOnPanel` | false | true if the arc moves with the panel |
| `kDeadbandDeg` | 5 | F2a: 5 deg off costs 0.4 % of the beam power |
| `kDarkToStowMs` | 60 000 | 1 min for lamp tests; 10 min outdoors |
| `kMinMoveGapMs` | 10 000 | stops hunting when the light flickers |
| `kServoTrimDeg` | 0 | from the inclinometer check |
| `kServoSlewDegPerS` | 30 | slower = smaller current spikes on the power bank |
| `kStartTracking` | false | servo stays off after boot until `servo auto`; true for unattended runs |
| `kSunResponsePower` | 1 | 2 if the photodiodes' response is narrower than a cosine (see `sun`) |
| `kStallBaseJ`, `kStallJPerDeg` | 1.5, 0.06 | servo stall protection (fault 8): energy allowed per move |

## Faults the control code reports

| Code | Meaning | What happens |
|---|---|---|
| 5 | sun-sensor arc fault: two or more channels saturated for 10 s, or readings that fit no light direction for 30 s | tracking holds |
| 7 | travel limit: a command outside +/-50 deg was clamped | shown for the minute |
| 8 | servo stall, overload or clutch slip: more energy drawn during a move than it allows (P_use), or the pot disagrees | servo switched off, tracking stops until `ctrl reset` |

The data link adds its own: 1/2/3 power sensor missing, 4 clock, 6 buffer
overflow, 9 uploads failing (docs/CONTRACT.md section 4).

## Known limitations

- Not yet run on hardware (see above).
- No light sleep yet: the board stays awake (about 2.9 Wh/day modelled,
  docs/energy/power_budget.md). Fine for lab tests; the energy result needs
  the sleep change next.
- The sun estimate assumes a cosine response: check it against the
  inclinometer and adjust `kSunSensorDeg` or the method (core/ast_sun.h).
- The panel angle is the commanded angle unless the pot is fitted, so a
  slipping clutch is only caught with the pot (or the inclinometer).
- The first move after flashing, or after a reset in the middle of a move,
  goes straight to its target at full speed; afterwards the angle of the last
  clean move is kept in flash and moves ramp at 30 deg/s.
- The stall check uses the 1 Hz P_use reading: with an INA219 (68 ms
  snapshots) a short jam can be missed; with an INA226 (1 s averages) it is
  caught.
- Many power banks switch off below about 50-100 mA. Check yours with the
  servo idle (bench item M13).
