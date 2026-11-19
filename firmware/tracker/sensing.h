// sensing.h - the photodiode arc: where is the light? Owner: CO (Solar Sensing
// and Tracking Control); starter code by AM, to be checked and tuned by CO.
//
// Four BPX65 photodiodes, each with a load resistor to GND and powered from
// 3V3 (NOT 5 V: no Nano ESP32 pin takes 5 V), on A0-A3. Each cycle reads every
// channel kSunSamples times, corrects it with the stored calibration and
// estimates the light direction with the cosine fit in core/ast_sun.h.
//
// Serial commands (type them in the serial monitor, 115200 baud, Newline):
//   sun              raw and corrected readings, the estimate and why
//   sun cal dark     all sensors covered: stores each channel's dark offset
//   sun cal flat     all sensors under the same light (paper diffuser): stores gains
//   sun cal clear    back to gain 1, offset 0
//   pot              panel-angle pot reading (if fitted)
#pragma once
#include <stdint.h>

namespace sensing {

struct Sun {
  bool valid = false;       // sun_deg can be used
  bool dark = false;        // too little light (night, lamp off)
  bool saturated = false;   // a channel is clipped at about 3.1 V (lower the load resistor)
  float sun_deg = 0.0f;     // light direction on the tracking axis, east negative
  float mv[4] = {0, 0, 0, 0};   // raw channel readings (mV)
  float total_mv = 0.0f;    // sum of corrected readings
  const char* why = "";     // reason when not valid
};

void begin();
Sun read_sun();
// Panel angle from the optional pot (config.h kPanelPotPin). false if not fitted.
bool panel_angle(float* deg);

}  // namespace sensing
