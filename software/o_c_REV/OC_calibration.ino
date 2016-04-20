/*
*
* calibration menu:
*
* enter by pressing left encoder button during start up; use encoder switches to navigate.
*
*/

#include "OC_calibration.h"

using OC::DAC;

static constexpr uint16_t DAC_OFFSET = 4890; // DAC offset, initial approx., ish --> -3.5V to 6V
static constexpr uint16_t _ADC_OFFSET = (uint16_t)((float)pow(2,OC::ADC::kAdcResolution)*0.6666667f); // ADC offset

namespace OC {
CalibrationStorage calibration_storage;
CalibrationData calibration_data;
};

static constexpr unsigned kCalibrationAdcSmoothing = 4;

const OC::CalibrationData kCalibrationDefaults = {
  // DAC
  { {
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535} },
  },
  // ADC
  { { _ADC_OFFSET, _ADC_OFFSET, _ADC_OFFSET, _ADC_OFFSET },
    0,  // pitch_cv_scale
    0   // pitch_cv_offset : unused
  },
  // display_offset
  SH1106_128x64_Driver::kDefaultOffset,
  OC_CALIBRATION_DEFAULT_FLAGS,
  0, 0 // reserved
};
//const uint16_t THEORY[OCTAVES+1] = {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535}; // in theory  

void calibration_reset() {
  memcpy(&OC::calibration_data, &kCalibrationDefaults, sizeof(OC::calibration_data));
  for (int i = 0; i < OCTAVES; ++i) {
    OC::calibration_data.dac.calibrated_octaves[0][i] += DAC_OFFSET;
    OC::calibration_data.dac.calibrated_octaves[1][i] += DAC_OFFSET;
    OC::calibration_data.dac.calibrated_octaves[2][i] += DAC_OFFSET;
    OC::calibration_data.dac.calibrated_octaves[3][i] += DAC_OFFSET;
  }
}

void calibration_load() {
  SERIAL_PRINTLN("CalibrationStorage: PAGESIZE=%u, PAGES=%u, LENGTH=%u",
                 OC::CalibrationStorage::PAGESIZE, OC::CalibrationStorage::PAGES, OC::CalibrationStorage::LENGTH);

  calibration_reset();
  if (!OC::calibration_storage.Load(OC::calibration_data)) {
#ifdef CALIBRATION_LOAD_LEGACY
    if (EEPROM.read(0x2) > 0) {
      SERIAL_PRINTLN("Calibration not loaded, non-zero data found, trying to import...");
      calibration_read_old();
    } else {
      SERIAL_PRINTLN("No calibration data found, using defaults");
    }
#else
    SERIAL_PRINTLN("No calibration data found, using defaults");
#endif
  } else {
    SERIAL_PRINTLN("Calibration data loaded...");
  }

  // Fix-up left-overs from development
  if (!OC::calibration_data.adc.pitch_cv_scale) {
    SERIAL_PRINTLN("NOTE: Pitch CV scale not set, using default...");
    OC::calibration_data.adc.pitch_cv_scale = OC::ADC::kDefaultPitchCVScale;
  }
}

void calibration_save() {
  SERIAL_PRINTLN("Saving calibration data...");
  OC::calibration_storage.Save(OC::calibration_data);
}

enum CALIBRATION_STEP {  
  HELLO,
  CENTER_DISPLAY,

  DAC_A_VOLT_3m, DAC_A_VOLT_2m, DAC_A_VOLT_1m, DAC_A_VOLT_0, DAC_A_VOLT_1, DAC_A_VOLT_2, DAC_A_VOLT_3, DAC_A_VOLT_4, DAC_A_VOLT_5, DAC_A_VOLT_6,
  DAC_B_VOLT_3m, DAC_B_VOLT_2m, DAC_B_VOLT_1m, DAC_B_VOLT_0, DAC_B_VOLT_1, DAC_B_VOLT_2, DAC_B_VOLT_3, DAC_B_VOLT_4, DAC_B_VOLT_5, DAC_B_VOLT_6,
  DAC_C_VOLT_3m, DAC_C_VOLT_2m, DAC_C_VOLT_1m, DAC_C_VOLT_0, DAC_C_VOLT_1, DAC_C_VOLT_2, DAC_C_VOLT_3, DAC_C_VOLT_4, DAC_C_VOLT_5, DAC_C_VOLT_6,
  DAC_D_VOLT_3m, DAC_D_VOLT_2m, DAC_D_VOLT_1m, DAC_D_VOLT_0, DAC_D_VOLT_1, DAC_D_VOLT_2, DAC_D_VOLT_3, DAC_D_VOLT_4, DAC_D_VOLT_5, DAC_D_VOLT_6,

  CV_OFFSET,
  CV_OFFSET_0, CV_OFFSET_1, CV_OFFSET_2, CV_OFFSET_3,
  ADC_PITCH_C2, ADC_PITCH_C4,
  CALIBRATION_EXIT,
  CALIBRATION_STEP_LAST,
  CALIBRATION_STEP_FINAL = ADC_PITCH_C4
};  

enum CALIBRATION_TYPE {
  CALIBRATE_NONE,
  CALIBRATE_OCTAVE,
  CALIBRATE_ADC_TRIMMER,
  CALIBRATE_ADC_OFFSET,
  CALIBRATE_ADC_1V,
  CALIBRATE_ADC_3V,
  CALIBRATE_DISPLAY
};

struct CalibrationStep {
  CALIBRATION_STEP step;
  const char *title;
  const char *message;
  const char *help; // optional
  const char *footer;

  CALIBRATION_TYPE calibration_type;
  int index;

  const char * const *value_str; // if non-null, use these instead of encoder value
  int min, max;
};

DAC_CHANNEL step_to_channel(int step) {
  if (step >= DAC_D_VOLT_3m) return DAC_CHANNEL_D;
  if (step >= DAC_C_VOLT_3m) return DAC_CHANNEL_C;
  if (step >= DAC_B_VOLT_3m) return DAC_CHANNEL_B;
  /*if (step >= DAC_B_VOLT_3m)*/ return DAC_CHANNEL_A;
}

struct CalibrationState {
  CALIBRATION_STEP step;
  const CalibrationStep *current_step;
  int encoder_value;

  SmoothedValue<uint32_t, kCalibrationAdcSmoothing> adc_sum;

  uint16_t adc_1v;
  uint16_t adc_3v;
};

OC::DigitalInputDisplay digital_input_displays[4];

// 128/6=21                  |                     |
const char *start_footer   = "              [START]";
const char *end_footer     = "[PREV]         [EXIT]";
const char *default_footer = "[PREV]         [NEXT]";
const char *default_help_r = "[R] => Adjust";
const char *select_help    = "[R] => Select";

const CalibrationStep calibration_steps[CALIBRATION_STEP_LAST] = {
  { HELLO, "O&C Calibration", "Use defaults? ", select_help, start_footer, CALIBRATE_NONE, 0, OC::Strings::no_yes, 0, 1 },
  { CENTER_DISPLAY, "Center Display", "Pixel offset ", default_help_r, default_footer, CALIBRATE_DISPLAY, 0, nullptr, 0, 2 },

  { DAC_A_VOLT_3m, "DAC A -3 volts", "-> -3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_2m, "DAC A -2 volts", "-> -2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_1m, "DAC A -1 volts", "-> -1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_0, "DAC A 0 volts", "->  0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_1, "DAC A 1 volts", "->  1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_2, "DAC A 2 volts", "->  2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_3, "DAC A 3 volts", "->  3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_4, "DAC A 4 volts", "->  4.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 4, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_5, "DAC A 5 volts", "->  5.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 5, nullptr, 0, DAC::MAX_VALUE },
  { DAC_A_VOLT_6, "DAC A 6 volts", "->  6.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 6, nullptr, 0, DAC::MAX_VALUE },

  { DAC_B_VOLT_3m, "DAC B -3 volts", "-> -3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_2m, "DAC B -2 volts", "-> -2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_1m, "DAC B -1 volts", "-> -1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_0, "DAC B 0 volts", "->  0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_1, "DAC B 1 volts", "->  1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_2, "DAC B 2 volts", "->  2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_3, "DAC B 3 volts", "->  3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_4, "DAC B 4 volts", "->  4.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 4, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_5, "DAC B 5 volts", "->  5.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 5, nullptr, 0, DAC::MAX_VALUE },
  { DAC_B_VOLT_6, "DAC B 6 volts", "->  6.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 6, nullptr, 0, DAC::MAX_VALUE },

  { DAC_C_VOLT_3m, "DAC C -3 volts", "-> -3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_2m, "DAC C -2 volts", "-> -2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_1m, "DAC C -1 volts", "-> -1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_0, "DAC C 0 volts", "->  0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_1, "DAC C 1 volts", "->  1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_2, "DAC C 2 volts", "->  2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_3, "DAC C 3 volts", "->  3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_4, "DAC C 4 volts", "->  4.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 4, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_5, "DAC C 5 volts", "->  5.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 5, nullptr, 0, DAC::MAX_VALUE },
  { DAC_C_VOLT_6, "DAC C 6 volts", "->  6.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 6, nullptr, 0, DAC::MAX_VALUE },

  { DAC_D_VOLT_3m, "DAC D -3 volts", "-> -3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_2m, "DAC D -2 volts", "-> -2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_1m, "DAC D -1 volts", "-> -1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, -1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_0, "DAC D 0 volts", "->  0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_1, "DAC D 1 volts", "->  1.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 1, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_2, "DAC D 2 volts", "->  2.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 2, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_3, "DAC D 3 volts", "->  3.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 3, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_4, "DAC D 4 volts", "->  4.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 4, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_5, "DAC D 5 volts", "->  5.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 5, nullptr, 0, DAC::MAX_VALUE },
  { DAC_D_VOLT_6, "DAC D 6 volts", "->  6.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 6, nullptr, 0, DAC::MAX_VALUE },

  { CV_OFFSET, "CV offset", "", "Adjust CV trimpot", default_footer, CALIBRATE_ADC_TRIMMER, 0, nullptr, 0, 4095 },
  { CV_OFFSET_0, "ADC CV1", "ADC value at 0V", default_help_r, default_footer, CALIBRATE_ADC_OFFSET, ADC_CHANNEL_1, nullptr, 0, 4095 },
  { CV_OFFSET_1, "ADC CV2", "ADC value at 0V", default_help_r, default_footer, CALIBRATE_ADC_OFFSET, ADC_CHANNEL_2, nullptr, 0, 4095 },
  { CV_OFFSET_2, "ADC CV3", "ADC value at 0V", default_help_r, default_footer, CALIBRATE_ADC_OFFSET, ADC_CHANNEL_3, nullptr, 0, 4095 },
  { CV_OFFSET_3, "ADC CV4", "ADC value at 0V", default_help_r, default_footer, CALIBRATE_ADC_OFFSET, ADC_CHANNEL_4, nullptr, 0, 4095 },

  { ADC_PITCH_C2, "CV Scaling 1V", "CV1: Input 1V (C2)", "[R] Long press to set", default_footer, CALIBRATE_ADC_1V, 0, nullptr, 0, 0 },
  { ADC_PITCH_C4, "CV Scaling 3V", "CV1: Input 3V (C4)", "[R] Long press to set", default_footer, CALIBRATE_ADC_3V, 0, nullptr, 0, 0 },

  { CALIBRATION_EXIT, "Calibration complete", "Save values? ", select_help, end_footer, CALIBRATE_NONE, 0, OC::Strings::no_yes, 0, 1 }
};

/*     loop calibration menu until done       */
void OC::Ui::Calibrate() {

  // Calibration data should be loaded (or defaults) by now
  SERIAL_PRINTLN("Starting calibration...");

  CalibrationState calibration_state = {
    HELLO,
    &calibration_steps[HELLO],
    1,
  };
  calibration_state.adc_sum.set(_ADC_OFFSET);

  for (auto &did : digital_input_displays)
    did.Init();

  TickCount tick_count;
  tick_count.Init();

  encoder_enable_acceleration(CONTROL_ENCODER_R, true);

  bool calibration_complete = false;
  while (!calibration_complete) {

    uint32_t ticks = tick_count.Update();
    digital_input_displays[0].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_1>());
    digital_input_displays[1].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_2>());
    digital_input_displays[2].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_3>());
    digital_input_displays[3].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_4>());

    while (event_queue_.available()) {
      const UI::Event event = event_queue_.PullEvent();
      if (IgnoreEvent(event))
        continue;

      switch (event.control) {
        case CONTROL_BUTTON_L:
          if (calibration_state.step > CENTER_DISPLAY)
            calibration_state.step = static_cast<CALIBRATION_STEP>(calibration_state.step - 1);
          break;
        case CONTROL_BUTTON_R:
          // Special case these values to read, before moving to next step
          if (UI::EVENT_BUTTON_LONG_PRESS == event.type) {
            switch (calibration_state.current_step->step) {
              case ADC_PITCH_C2:
                calibration_state.adc_1v = OC::ADC::value(ADC_CHANNEL_1);
                break;
              case ADC_PITCH_C4:
                calibration_state.adc_3v = OC::ADC::value(ADC_CHANNEL_1);
                break;
              default: break;
            }
          }
          if (calibration_state.step < CALIBRATION_EXIT)
            calibration_state.step = static_cast<CALIBRATION_STEP>(calibration_state.step + 1);
          else
            calibration_complete = true;
          break;
        case CONTROL_ENCODER_L:
          if (calibration_state.step > HELLO) {
            calibration_state.step = static_cast<CALIBRATION_STEP>(calibration_state.step + event.value);
            CONSTRAIN(calibration_state.step, CENTER_DISPLAY, CALIBRATION_EXIT);
          }
          break;
        case CONTROL_ENCODER_R:
          calibration_state.encoder_value += event.value;
          break;
        case CONTROL_BUTTON_DOWN:
          SERIAL_PRINTLN("Reversing encoders...");
          calibration_data.reverse_encoders();
          reverse_encoders(calibration_data.encoders_reversed());
        default:
          break;
      }
    }

    const CalibrationStep *next_step = &calibration_steps[calibration_state.step];
    if (next_step != calibration_state.current_step) {
      SERIAL_PRINTLN("Step: %s (%d)", next_step->title, step_to_channel(next_step->step));
      // Special cases on exit current step
      switch (calibration_state.current_step->step) {
        case HELLO:
          if (calibration_state.encoder_value) {
            SERIAL_PRINTLN("Resetting to defaults...");
            calibration_reset();
          }
          break;
        case ADC_PITCH_C4:
          if (calibration_state.adc_1v && calibration_state.adc_3v) {
            OC::ADC::CalibratePitch(calibration_state.adc_1v, calibration_state.adc_3v);
            SERIAL_PRINTLN("ADC SCALE 1V=%d, 3V=%d -> %d",
                           calibration_state.adc_1v, calibration_state.adc_3v,
                           OC::calibration_data.adc.pitch_cv_scale);
          }
          break;

        default: break;
      }

      // Setup next step
      switch (next_step->calibration_type) {
      case CALIBRATE_OCTAVE:
        calibration_state.encoder_value =
            OC::calibration_data.dac.calibrated_octaves[step_to_channel(next_step->step)][next_step->index + DAC::kOctaveZero];
        break;
      case CALIBRATE_ADC_TRIMMER:
        calibration_state.adc_sum.set(adc_average());
        break;
      case CALIBRATE_ADC_OFFSET:
        calibration_state.encoder_value = OC::calibration_data.adc.offset[next_step->index];
        break;
      case CALIBRATE_DISPLAY:
        calibration_state.encoder_value = OC::calibration_data.display_offset;
        break;

      case CALIBRATE_ADC_1V:
      case CALIBRATE_ADC_3V:
        SERIAL_PRINTLN("offset=%d", OC::calibration_data.adc.offset[ADC_CHANNEL_1]);
        break;

      case CALIBRATE_NONE:
      default:
        if (CALIBRATION_EXIT != next_step->step)
          calibration_state.encoder_value = 0;
        else
          calibration_state.encoder_value = 1;
      }
      calibration_state.current_step = next_step;
    }

    calibration_update(calibration_state);
    calibration_draw(calibration_state);
  }

  if (calibration_state.encoder_value) {
    SERIAL_PRINTLN("Calibration complete");
    calibration_save();
  } else {
    SERIAL_PRINTLN("Calibration complete, not saving values...");
  }
}

void calibration_draw(const CalibrationState &state) {
  GRAPHICS_BEGIN_FRAME(true);
  const CalibrationStep *step = state.current_step;

  menu::DefaultTitleBar::Draw();
  graphics.print(step->title);

  weegfx::coord_t y = menu::CalcLineY(0);

  static constexpr weegfx::coord_t kValueX = menu::kDisplayWidth - 30;

  graphics.setPrintPos(menu::kIndentDx, y + 2);
  switch (step->calibration_type) {
    case CALIBRATE_OCTAVE:
      graphics.print(step->message);
      graphics.setPrintPos(kValueX, y + 2);
      graphics.print((int)state.encoder_value, 5);
      menu::DrawEditIcon(kValueX, y, state.encoder_value, step->min, step->max);
      break;

    case CALIBRATE_ADC_TRIMMER:
      graphics.print(_ADC_OFFSET, 4);
      graphics.print(" == ");
      graphics.print(state.adc_sum.value() >> 2, 4);
      break;

    case CALIBRATE_ADC_OFFSET:
      graphics.print(step->message);
      graphics.setPrintPos(kValueX, y + 2);
      graphics.print((int)OC::ADC::value(static_cast<ADC_CHANNEL>(step->index)), 5);
      menu::DrawEditIcon(kValueX, y, state.encoder_value, step->min, step->max);
      break;

    case CALIBRATE_DISPLAY:
      graphics.print(step->message);
      graphics.setPrintPos(kValueX, y + 2);
      graphics.pretty_print((int)state.encoder_value, 2);
      menu::DrawEditIcon(kValueX, y, state.encoder_value, step->min, step->max);
      graphics.drawFrame(0, 0, 128, 64);
      break;

    case CALIBRATE_ADC_1V:
    case CALIBRATE_ADC_3V:
      graphics.setPrintPos(menu::kIndentDx, y + 2);
      graphics.print(step->message);
      y += menu::kMenuLineH;
      graphics.setPrintPos(menu::kIndentDx, y + 2);
      graphics.print((int)OC::ADC::value(ADC_CHANNEL_1), 2);
      break;

    case CALIBRATE_NONE:
    default:
      graphics.setPrintPos(menu::kIndentDx, y + 2);
      graphics.print(step->message);
      if (step->value_str)
        graphics.print(step->value_str[state.encoder_value]);
      break;
  }

  y += menu::kMenuLineH;
  graphics.setPrintPos(menu::kIndentDx, y + 2);
  if (step->help)
    graphics.print(step->help);

  weegfx::coord_t x = menu::kDisplayWidth - 22;
  y = 2;
  for (int input = OC::DIGITAL_INPUT_1; input < OC::DIGITAL_INPUT_LAST; ++input) {
    uint8_t state = (digital_input_displays[input].getState() + 3) >> 2;
    if (state)
      graphics.drawBitmap8(x, y, 4, OC::bitmap_gate_indicators_8 + (state << 2));
    x += 5;
  }

  graphics.drawStr(1, menu::kDisplayHeight - menu::kFontHeight - 3, step->footer);

  static constexpr uint16_t step_width = (menu::kDisplayWidth << 8 ) / (CALIBRATION_STEP_LAST - 1);
  graphics.drawRect(0, menu::kDisplayHeight - 2, (state.step * step_width) >> 8, 2);

  GRAPHICS_END_FRAME();
}

/* DAC output etc */ 

void calibration_update(CalibrationState &state) {

  CONSTRAIN(state.encoder_value, state.current_step->min, state.current_step->max);
  const CalibrationStep *step = state.current_step;

  switch (step->calibration_type) {
    case CALIBRATE_NONE:
      DAC::set_all_octave(0);
      break;
    case CALIBRATE_OCTAVE:
      OC::calibration_data.dac.calibrated_octaves[step_to_channel(step->step)][step->index + DAC::kOctaveZero] =
        state.encoder_value;
      DAC::set_all_octave(step->index);
      break;
    case CALIBRATE_ADC_TRIMMER:
      state.adc_sum.push(adc_average());
      DAC::set_all_octave(0);
      break;
    case CALIBRATE_ADC_OFFSET:
      OC::calibration_data.adc.offset[step->index] = state.encoder_value;
      DAC::set_all_octave(0);
      break;
    case CALIBRATE_ADC_1V:
      DAC::set_all_octave(1);
      break;
    case CALIBRATE_ADC_3V:
      DAC::set_all_octave(3);
      break;
    case CALIBRATE_DISPLAY:
      OC::calibration_data.display_offset = state.encoder_value;
      display::AdjustOffset(OC::calibration_data.display_offset);
      break;
  }
}

/* misc */ 

uint32_t adc_average() {
  delay(OC_CORE_TIMER_RATE + 1);

  return
    OC::ADC::smoothed_raw_value(ADC_CHANNEL_1) + OC::ADC::smoothed_raw_value(ADC_CHANNEL_2) +
    OC::ADC::smoothed_raw_value(ADC_CHANNEL_3) + OC::ADC::smoothed_raw_value(ADC_CHANNEL_4);
}

#ifdef CALIBRATION_LOAD_LEGACY
/* read settings from original O&C */
void calibration_read_old() {
  
   delay(1000);
   uint8_t byte0, byte1, adr;
   
   adr = 0; 
   SERIAL_PRINTLN("Loading original O&C calibration from eeprom:");
   
   for (int i = 0; i < OCTAVES; i++) {  
  
       byte0 = EEPROM.read(adr);
       adr++;
       byte1 = EEPROM.read(adr);
       adr++;
       OC::calibration_data.dac.octaves[i] = (uint16_t)(byte0 << 8) + byte1;
       SERIAL_PRINTLN(" OCTAVE %2d: %u", i, OC::calibration_data.dac.octaves[i]);
   }
   
   uint16_t _offset[ADC_CHANNEL_LAST];
   
   for (int i = 0; i < ADC_CHANNEL_LAST; i++) {  
  
       byte0 = EEPROM.read(adr);
       adr++;
       byte1 = EEPROM.read(adr);
       adr++;
       _offset[i] = (uint16_t)(byte0 << 8) + byte1;
       SERIAL_PRINTLN(" ADC %d: %u", i, _offset[i]);
   }
   
   OC::calibration_data.adc.offset[ADC_CHANNEL_1] = _offset[0];
   OC::calibration_data.adc.offset[ADC_CHANNEL_2] = _offset[1];
   OC::calibration_data.adc.offset[ADC_CHANNEL_3] = _offset[2];
   OC::calibration_data.adc.offset[ADC_CHANNEL_4] = _offset[3];
   SERIAL_PRINTLN("......");
}  
#endif
