#include "as11_settings.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "as11_rpc.h"
#ifdef ARDUINO
#include "memory_manager.h"
#endif
#include "string_util.h"

namespace aircannect {
namespace {

void *settings_alloc_large(size_t size) {
#ifdef ARDUINO
    return Memory::alloc_large(size);
#else
    return malloc(size);
#endif
}

void settings_free(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

#define MODE_BIT(mode) (static_cast<uint16_t>(1u << (mode)))
#define MODES_ALL 0x07FFu
#define MODES_CPAP MODE_BIT(0)
#define MODES_AUTO (MODE_BIT(1) | MODE_BIT(2))
#define MODES_BILEVEL (MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(5) | \
                       MODE_BIT(6) | MODE_BIT(9) | MODE_BIT(10))
#define MODES_VAUTO MODE_BIT(6)
#define MODES_ASV MODE_BIT(7)
#define MODES_ASVAUTO MODE_BIT(8)
#define MODES_IVAPS MODE_BIT(9)

template <size_t N>
constexpr uint8_t option_count(const char *const (&)[N]) {
    return static_cast<uint8_t>(N);
}

const char *const MODE_OPTIONS[] = {
    "CPAP", "AutoSet", "AutoSet For Her", "S", "ST", "T",
    "VAuto", "ASV", "ASVAuto", "iVAPS", "PAC",
};
const char *const ON_OFF_OPTIONS[] = {"Off", "On"};
const char *const YES_NO_OPTIONS[] = {"No", "Yes"};
const char *const RAMP_OPTIONS[] = {"Off", "On", "Auto"};
const char *const AUTO_MANUAL_OPTIONS[] = {"Auto", "Manual"};
const char *const STANDARD_SOFT_OPTIONS[] = {"Standard", "Soft"};
const char *const EPR_TYPE_OPTIONS[] = {"RampOnly", "FullTime"};
const char *const TEMPERATURE_UNIT_OPTIONS[] = {"Celsius", "Fahrenheit"};
const char *const PATIENT_VIEW_OPTIONS[] = {"Simple", "Full"};
const char *const SENSITIVITY_OPTIONS[] = {
    "Very Low", "Low", "Medium", "High", "Very High",
};
const char *const MASK_OPTIONS[] = {
    "Pillows", "Full Face", "Nasal", "Pediatric",
};
const char *const TUBE_OPTIONS[] = {
    "15mmNonHeated", "19mm", "15mmHeated",
};

struct KnownVarAlias {
    const char *name;
    const char *alias;
};

const KnownVarAlias KNOWN_VAR_ALIASES[] = {
    {"ActiveTherapyProfile", "_MOP"},

    {"ASV-MaxPressureSupport", "_XC2"},
    {"ASV-MinPressureSupport", "_XC3"},
    {"ASV-StartPressure", "_XC0"},
    {"ASV-TargetExpiratoryPressure", "_XC1"},
    {"ASVAuto-MaxExpiratoryPressure", "_XD1"},
    {"ASVAuto-MaxPressureSupport", "_XD3"},
    {"ASVAuto-MinExpiratoryPressure", "_XD2"},
    {"ASVAuto-MinPressureSupport", "_XD4"},
    {"ASVAuto-StartPressure", "_XD0"},
    {"AutoSet-MaxPressure", "_MPA"},
    {"AutoSet-MinPressure", "_MPI"},
    {"AutoSet-StartPressure", "_STU"},
    {"Cpap-SetPressure", "_IPC"},
    {"Cpap-StartPressure", "_STP"},
    {"Cpap-TriggerSensitivity", "_C11"},
    {"HerAuto-MaxPressure", "_HMA"},
    {"HerAuto-MinPressure", "_HMI"},
    {"HerAuto-StartPressure", "_HSP"},
    {"PAC-RiseTime", "_PA4"},
    {"PAC-SetInspiratoryTime", "_PA5"},
    {"PAC-SetRespiratoryRate", "_PA6"},
    {"PAC-StartPressure", "_PA0"},
    {"PAC-TargetExpiratoryPressure", "_PA2"},
    {"PAC-TargetInspiratoryPressure", "_PA1"},
    {"PAC-TriggerSensitivity", "_PA7"},
    {"ST-CycleSensitivity", "_XAB"},
    {"ST-RiseTime", "_XAA"},
    {"ST-SetMaxInspiratoryTime", "_XA7"},
    {"ST-SetMinInspiratoryTime", "_XA8"},
    {"ST-SetRespiratoryRate", "_XA6"},
    {"ST-StartPressure", "_XA3"},
    {"ST-TargetExpiratoryPressure", "_XA2"},
    {"ST-TargetInspiratoryPressure", "_XA1"},
    {"ST-TriggerSensitivity", "_ZU1"},
    {"Spont-CycleSensitivity", "_Z12"},
    {"Spont-RiseTime", "_Z10"},
    {"Spont-SetMaxInspiratoryTime", "_ZZ7"},
    {"Spont-SetMinInspiratoryTime", "_ZZ8"},
    {"Spont-StartPressure", "_ZZ3"},
    {"Spont-TargetExpiratoryPressure", "_ZZ2"},
    {"Spont-TargetInspiratoryPressure", "_ZZ1"},
    {"Spont-TriggerSensitivity", "_Z11"},
    {"Timed-RiseTime", "_XB7"},
    {"Timed-SetInspiratoryTime", "_XB5"},
    {"Timed-SetRespiratoryRate", "_XB4"},
    {"Timed-StartPressure", "_XB0"},
    {"Timed-TargetExpiratoryPressure", "_XB2"},
    {"Timed-TargetInspiratoryPressure", "_XB1"},
    {"VAuto-CycleSensitivity", "_XE7"},
    {"VAuto-MaxInspiratoryPressure", "_XE1"},
    {"VAuto-MinExpiratoryPressure", "_XE2"},
    {"VAuto-SetMaxInspiratoryTime", "_XE4"},
    {"VAuto-SetMinInspiratoryTime", "_XE5"},
    {"VAuto-SetPressureSupport", "_XE3"},
    {"VAuto-StartPressure", "_XE0"},
    {"VAuto-TriggerSensitivity", "_XE6"},
    {"iVAPS-AutoEPAPEnable", "_IEU"},
    {"iVAPS-CycleSensitivity", "_VCS"},
    {"iVAPS-MaxExpiratoryPressure", "_IMX"},
    {"iVAPS-MaxPressureSupport", "_WPA"},
    {"iVAPS-MinExpiratoryPressure", "_IMN"},
    {"iVAPS-MinPressureSupport", "_WPM"},
    {"iVAPS-RiseTime", "_IRT"},
    {"iVAPS-SetMaxInspiratoryTime", "_IVX"},
    {"iVAPS-SetMinInspiratoryTime", "_IVN"},
    {"iVAPS-StartPressure", "_IVS"},
    {"iVAPS-TargetExpiratoryPressure", "_EPI"},
    {"iVAPS-TriggerSensitivity", "_VTS"},

    {"AntiBacterialFilter", "_ABF"},
    {"AutoSetComfort", "_AFC"},
    {"CareCheckInAvailable", "_CCA"},
    {"CareCheckToggle", "_MAI"},
    {"ClinicalConfirmation", "_CFC"},
    {"ClimateControl", "_CCO"},
    {"ConfirmStopEnable", "_SCF"},
    {"DisplayAHI", "_DAH"},
    {"EprEnable", "_EPX"},
    {"EprEnablePatientAccess", "_EPA"},
    {"EprPressure", "_EPR"},
    {"EprType", "_EPT"},
    {"ExternalHumidifier", "_EXH"},
    {"HeatedTubeSettingEnable", "_HTX"},
    {"HeatedTubeTemperature", "_HTS"},
    {"HumidifierLevel", "_HMS"},
    {"HumidifierSettingEnable", "_HMX"},
    {"MaskType", "_MSK"},
    {"MaskSenseToggle", "_MKD"},
    {"MyAirScreens", "_MAS"},
    {"PatientView", "_ACC"},
    {"RampEnable", "_RMA"},
    {"RampEnablePatientAccess", "_RPE"},
    {"RampTime", "_RMT"},
    {"SmartStart", "_SST"},
    {"SmartStop", "_SSP"},
    {"SplashScreenDisplaySelection", "_SSE"},
    {"TemperatureUnit", "_TMU"},
    {"TherapyLEDAlwaysOn", "_TLF"},
    {"TotalUsedHoursDisplayToggle", "_TUD"},
    {"TubeType", "_TBT"},
    {"SoundcheckFeatureToggle", "_SCO"},
    {"SoundcheckRunFrequency", "_SCK"},
};

const As11SettingDef SETTINGS[] = {
    {"TherapyMode", "TherapyMode", nullptr, nullptr, "_MOP",
     "Therapy Mode", "clinical", As11SettingKind::Enum,
     0, 10, 1, MODE_OPTIONS, option_count(MODE_OPTIONS), MODES_ALL, 1, 0},

    {"SetPressure", "SetPressure", nullptr, nullptr, nullptr,
     "Set Pressure", "mode", As11SettingKind::Number,
     4.0f, 20.0f, 0.2f, nullptr, 0, MODES_CPAP, 50, 1},
    {"MaxPressure", "MaxPressure", nullptr, nullptr, nullptr,
     "Max Pressure", "mode", As11SettingKind::Number,
     4.0f, 20.0f, 0.2f, nullptr, 0, MODES_AUTO, 50, 1},
    {"MinPressure", "MinPressure", nullptr, nullptr, nullptr,
     "Min Pressure", "mode", As11SettingKind::Number,
     4.0f, 20.0f, 0.2f, nullptr, 0, MODES_AUTO, 50, 1},
    {"TargetInspiratoryPressure", "TargetInspiratoryPressure", nullptr,
     nullptr, nullptr, "IPAP", "mode", As11SettingKind::Number,
     4.0f, 30.0f, 0.2f, nullptr, 0,
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(5) | MODE_BIT(10), 50, 1},
    {"TargetExpiratoryPressure", "TargetExpiratoryPressure", nullptr,
     nullptr, nullptr, "EPAP", "mode", As11SettingKind::Number,
     4.0f, 25.0f, 0.2f, nullptr, 0,
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(5) | MODE_BIT(10), 50, 1},
    {"MaxInspiratoryPressure", "MaxInspiratoryPressure", nullptr, nullptr,
     nullptr, "Max IPAP", "mode", As11SettingKind::Number,
     4.0f, 30.0f, 0.2f, nullptr, 0, MODES_VAUTO, 50, 1},
    {"MinExpiratoryPressure", "MinExpiratoryPressure", nullptr, nullptr,
     nullptr, "Min EPAP", "mode", As11SettingKind::Number,
     4.0f, 25.0f, 0.2f, nullptr, 0, MODES_VAUTO, 50, 1},
    {"SetPressureSupport", "SetPressureSupport", nullptr, nullptr,
     nullptr, "Pressure Support", "mode", As11SettingKind::Number,
     0.0f, 20.0f, 0.2f, nullptr, 0, MODES_VAUTO, 50, 1},
    {"TargetExpiratoryPressure", "TargetExpiratoryPressure", nullptr,
     nullptr, nullptr, "EPAP", "mode", As11SettingKind::Number,
     4.0f, 25.0f, 0.2f, nullptr, 0, MODES_ASV, 50, 1},
    {"MinPressureSupport", "MinPressureSupport", nullptr, nullptr,
     nullptr, "Min PS", "mode", As11SettingKind::Number,
     0.0f, 20.0f, 0.2f, nullptr, 0, MODES_ASV, 50, 1},
    {"MaxPressureSupport", "MaxPressureSupport", nullptr, nullptr,
     nullptr, "Max PS", "mode", As11SettingKind::Number,
     0.0f, 20.0f, 0.2f, nullptr, 0, MODES_ASV, 50, 1},
    {"MinExpiratoryPressure", "MinExpiratoryPressure", nullptr, nullptr,
     nullptr, "Min EPAP", "mode", As11SettingKind::Number,
     4.0f, 25.0f, 0.2f, nullptr, 0, MODES_ASVAUTO, 50, 1},
    {"MaxExpiratoryPressure", "MaxExpiratoryPressure", nullptr, nullptr,
     nullptr, "Max EPAP", "mode", As11SettingKind::Number,
     4.0f, 25.0f, 0.2f, nullptr, 0, MODES_ASVAUTO, 50, 1},
    {"MinPressureSupport", "MinPressureSupport", nullptr, nullptr,
     nullptr, "Min PS", "mode", As11SettingKind::Number,
     0.0f, 20.0f, 0.2f, nullptr, 0, MODES_ASVAUTO, 50, 1},
    {"MaxPressureSupport", "MaxPressureSupport", nullptr, nullptr,
     nullptr, "Max PS", "mode", As11SettingKind::Number,
     0.0f, 20.0f, 0.2f, nullptr, 0, MODES_ASVAUTO, 50, 1},

    {"SetRespiratoryRate", "SetRespiratoryRate", nullptr, nullptr,
     nullptr, "Resp. Rate", "timing", As11SettingKind::Number,
     0, 50, 0.2f, nullptr, 0, MODE_BIT(4) | MODE_BIT(5) | MODE_BIT(10),
     5, 1},
    {"SetMinInspiratoryTime", "SetMinInspiratoryTime", nullptr, nullptr,
     nullptr, "Ti Min", "timing", As11SettingKind::Number,
     0.1f, 4.0f, 0.02f, nullptr, 0,
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(6) | MODE_BIT(9), 50, 2},
    {"SetMaxInspiratoryTime", "SetMaxInspiratoryTime", nullptr, nullptr,
     nullptr, "Ti Max", "timing", As11SettingKind::Number,
     0.1f, 4.0f, 0.02f, nullptr, 0,
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(6) | MODE_BIT(9), 50, 2},
    {"SetInspiratoryTime", "SetInspiratoryTime", nullptr, nullptr,
     nullptr, "Ti", "timing", As11SettingKind::Number,
     0.1f, 4.0f, 0.02f, nullptr, 0,
     MODE_BIT(5) | MODE_BIT(10), 50, 2},
    {"RiseTime", "RiseTime", nullptr, nullptr, nullptr,
     "Rise Time", "timing", As11SettingKind::Number,
     0, 900, 1, nullptr, 0,
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(5) | MODE_BIT(9) | MODE_BIT(10),
     1, 0},
    {"TriggerSensitivity", "TriggerSensitivity", nullptr, nullptr,
     nullptr, "Trigger Sens.", "timing", As11SettingKind::Enum,
     0, 4, 1, SENSITIVITY_OPTIONS, option_count(SENSITIVITY_OPTIONS),
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(6) | MODE_BIT(9) | MODE_BIT(10),
     1, 0},
    {"CycleSensitivity", "CycleSensitivity", nullptr, nullptr, nullptr, "Cycle Sens.", "timing", As11SettingKind::Enum,
     0, 4, 1, SENSITIVITY_OPTIONS, option_count(SENSITIVITY_OPTIONS),
     MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(6) | MODE_BIT(9), 1, 0},

    {"StartPressure", "StartPressure", nullptr, nullptr, nullptr,
     "Start Pressure", "comfort", As11SettingKind::Number,
     4.0f, 20.0f, 0.2f, nullptr, 0, MODES_CPAP | MODES_AUTO, 50, 1},
    {"StartPressure", "StartPressure", nullptr, nullptr, nullptr,
     "Start EPAP", "comfort", As11SettingKind::Number,
     4.0f, 25.0f, 0.2f, nullptr, 0,
     MODES_BILEVEL | MODES_ASV | MODES_ASVAUTO, 50, 1},
    {"AutoSetComfort", nullptr, "ComfortFeature", "AutoSetComfort",
     nullptr, "Response", "comfort", As11SettingKind::Enum,
     0, 1, 1, STANDARD_SOFT_OPTIONS, option_count(STANDARD_SOFT_OPTIONS),
     MODE_BIT(1), 1, 0, ON_OFF_OPTIONS},
    {"RampEnable", nullptr, "AutoRampFeature", "RampEnable", nullptr, "Ramp", "comfort", As11SettingKind::Enum,
     0, 2, 1, RAMP_OPTIONS, option_count(RAMP_OPTIONS), MODES_ALL, 1, 0},
    {"RampEnablePatientAccess", nullptr, "AutoRampFeature",
     "RampEnablePatientAccess", nullptr, "Ramp Patient Access", "comfort",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"RampTime", nullptr, "AutoRampFeature", "RampTime", nullptr,
     "Ramp Time", "comfort", As11SettingKind::Number,
     5, 45, 1, nullptr, 0, MODES_ALL, 1, 0},
    {"EprEnable", nullptr, "EprFeature", "EprEnable", nullptr,
     "EPR", "comfort", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS),
     MODES_CPAP | MODES_AUTO, 1, 0},
    {"EprPressure", nullptr, "EprFeature", "EprPressure", nullptr,
     "EPR Level", "comfort", As11SettingKind::Number,
     0, 3, 1, nullptr, 0, MODES_CPAP | MODES_AUTO, 50, 0},
    {"EprEnablePatientAccess", nullptr, "EprFeature",
     "EprEnablePatientAccess", nullptr, "EPR Patient Access", "comfort",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_CPAP | MODES_AUTO, 1, 0},
    {"EprType", nullptr, "EprFeature", "EprType", nullptr,
     "EPR Type", "comfort", As11SettingKind::Enum,
     0, 1, 1, EPR_TYPE_OPTIONS, option_count(EPR_TYPE_OPTIONS),
     MODES_CPAP | MODES_AUTO, 1, 0},
    {"SmartStart", nullptr, "SmartStartStopFeature", "SmartStart",
     nullptr, "SmartStart", "comfort", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS), MODES_ALL, 1,
     0},
    {"SmartStop", nullptr, "SmartStartStopFeature", "SmartStop",
     nullptr, "SmartStop", "comfort", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS), MODES_ALL, 1,
     0},

    {"MaskType", nullptr, "CircuitFeature", "MaskType", nullptr,
     "Mask Type", "circuit", As11SettingKind::Enum,
     0, 3, 1, MASK_OPTIONS, option_count(MASK_OPTIONS), MODES_ALL, 1, 0},
    {"TubeType", nullptr, "CircuitFeature", "TubeType", nullptr,
     "Tube Type", "circuit", As11SettingKind::Enum,
     0, 2, 1, TUBE_OPTIONS, option_count(TUBE_OPTIONS), MODES_ALL, 1, 0},
    {"AntiBacterialFilter", nullptr, "CircuitFeature",
     "AntiBacterialFilter", nullptr, "AB Filter", "circuit",
     As11SettingKind::Enum, 0, 1, 1, YES_NO_OPTIONS,
     option_count(YES_NO_OPTIONS), MODES_ALL, 1, 0},
    {"MaskSenseToggle", nullptr, "MaskSenseFeature", "MaskSenseToggle",
     nullptr, "Mask Sense", "circuit", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS), MODES_ALL, 1,
     0},

    {"ClimateControl", nullptr, "ClimateFeature", "ClimateControl",
     nullptr, "Climate Control", "climate", As11SettingKind::Enum,
     0, 1, 1, AUTO_MANUAL_OPTIONS, option_count(AUTO_MANUAL_OPTIONS),
     MODES_ALL, 1, 0},
    {"HumidifierSettingEnable", nullptr, "ClimateFeature",
     "HumidifierSettingEnable", nullptr, "Humidity", "climate",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"HumidifierLevel", nullptr, "ClimateFeature", "HumidifierLevel",
     nullptr, "Humidity Level", "climate", As11SettingKind::Number,
     0, 8, 1, nullptr, 0, MODES_ALL, 1, 0},
    {"HeatedTubeSettingEnable", nullptr, "ClimateFeature",
     "HeatedTubeSettingEnable", nullptr, "Tube Heating", "climate",
     As11SettingKind::Enum, 0, 2, 1, RAMP_OPTIONS,
     option_count(RAMP_OPTIONS), MODES_ALL, 1, 0},
    {"HeatedTubeTemperature", nullptr, "ClimateFeature",
     "HeatedTubeTemperature", nullptr, "Tube Temp C", "climate",
     As11SettingKind::Number, 15.0f, 30.0f, 0.1f, nullptr, 0, MODES_ALL,
     10, 0},
    {"ExternalHumidifier", nullptr, "ClimateFeature", "ExternalHumidifier",
     nullptr, "External Humidifier", "climate", As11SettingKind::Enum,
     0, 1, 1, YES_NO_OPTIONS, option_count(YES_NO_OPTIONS), MODES_ALL, 1,
     0},

    {"ConfirmStopEnable", nullptr, "ConfirmStopFeature", "ConfirmStopEnable",
     nullptr, "Confirm Stop", "preferences", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS), MODES_ALL, 1,
     0},
    {"TemperatureUnit", nullptr, "TemperatureFeature", "TemperatureUnit",
     nullptr, "Temperature Unit", "preferences", As11SettingKind::Enum,
     0, 1, 1, TEMPERATURE_UNIT_OPTIONS, option_count(TEMPERATURE_UNIT_OPTIONS),
     MODES_ALL, 1, 0},
    {"PatientView", nullptr, "PatientViewFeature", "PatientView",
     nullptr, "Patient View", "preferences", As11SettingKind::Enum,
     0, 1, 1, PATIENT_VIEW_OPTIONS, option_count(PATIENT_VIEW_OPTIONS),
     MODES_ALL, 1, 0},
    {"DisplayAHI", nullptr, "PatientViewFeature", "DisplayAHI",
     nullptr, "Display AHI", "preferences", As11SettingKind::Enum,
     0, 1, 1, YES_NO_OPTIONS, option_count(YES_NO_OPTIONS), MODES_ALL, 1,
     0},
    {"TherapyLEDAlwaysOn", nullptr, "TherapyLEDFeature",
     "TherapyLEDAlwaysOn", nullptr, "Therapy LED", "preferences",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"TotalUsedHoursDisplayToggle", nullptr, "DisplayFeature",
     "TotalUsedHoursDisplayToggle", nullptr, "Show Used Hours", "preferences",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"CareCheckInAvailable", nullptr, "DisplayFeature",
     "CareCheckInAvailable", nullptr, "Care Check-In", "preferences",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"MyAirScreens", nullptr, "DisplayFeature", "MyAirScreens",
     nullptr, "myAir Screens", "preferences", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS), MODES_ALL, 1,
     0},
    {"ClinicalConfirmation", nullptr, "DisplayFeature",
     "ClinicalConfirmation", nullptr, "Clinical Confirmation", "preferences",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"CareCheckToggle", nullptr, "CareCheckFeature", "CareCheckToggle",
     nullptr, "Care Check", "preferences", As11SettingKind::Enum,
     0, 1, 1, ON_OFF_OPTIONS, option_count(ON_OFF_OPTIONS), MODES_ALL, 1,
     0},
    {"SoundcheckFeatureToggle", nullptr, "DeviceHealthFeature",
     "SoundcheckFeatureToggle", nullptr, "Sound Check", "preferences",
     As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
    {"SoundcheckRunFrequency", nullptr, "DeviceHealthFeature",
     "SoundcheckRunFrequency", nullptr, "Sound Check Frequency",
     "preferences", As11SettingKind::Enum, 0, 1, 1, ON_OFF_OPTIONS,
     option_count(ON_OFF_OPTIONS), MODES_ALL, 1, 0},
};

constexpr size_t SETTINGS_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
static_assert(SETTINGS_COUNT <= As11SettingsState::MaxSettings,
              "As11SettingsState value storage too small");

bool json_is_number(JsonVariantConst value) {
    return value.is<int>() || value.is<unsigned int>() ||
           value.is<long>() || value.is<unsigned long>() ||
           value.is<long long>() || value.is<unsigned long long>() ||
           value.is<float>() || value.is<double>();
}

std::string value_to_string(JsonVariantConst value) {
    if (value.is<const char *>()) return value.as<const char *>();
    if (value.is<bool>()) return value.as<bool>() ? "true" : "false";
    if (value.is<int>()) return std::to_string(value.as<int>());
    if (value.is<unsigned int>()) return std::to_string(value.as<unsigned int>());
    if (value.is<long>()) return std::to_string(value.as<long>());
    if (value.is<unsigned long>()) return std::to_string(value.as<unsigned long>());
    if (value.is<long long>()) return std::to_string(value.as<long long>());
    if (value.is<unsigned long long>()) {
        return std::to_string(value.as<unsigned long long>());
    }
    if (value.is<float>() || value.is<double>()) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.3f", value.as<double>());
        char *end = buf + strlen(buf);
        while (end > buf && end[-1] == '0') *--end = 0;
        if (end > buf && end[-1] == '.') *--end = 0;
        return buf;
    }
    return "";
}

bool parse_number(const std::string &text, double &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    value = strtod(text.c_str(), &end);
    return end && *end == 0;
}

bool parse_iso_seconds(const char *text, double &seconds) {
    if (!text || strncmp(text, "PT", 2) != 0) return false;
    const char *start = text + 2;
    char *end = nullptr;
    seconds = strtod(start, &end);
    return end && end != start && end[0] == 'S' && end[1] == 0;
}

std::string compact_mode_key(const char *value) {
    std::string out;
    if (!value) return out;
    while (*value) {
        const unsigned char c = static_cast<unsigned char>(*value++);
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(static_cast<char>(tolower(c)));
    }
    const char suffix[] = "profile";
    const size_t suffix_len = sizeof(suffix) - 1;
    if (out.size() > suffix_len &&
        out.compare(out.size() - suffix_len, suffix_len, suffix) == 0) {
        out.resize(out.size() - suffix_len);
    }
    if (out == "autosetforher") return "autosether";
    return out;
}

bool parse_int_text(const std::string &text, int &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (!end || *end != 0) return false;
    value = static_cast<int>(parsed);
    return true;
}

int option_index_of(const As11SettingDef &def, const char *value) {
    if (!value) return -1;
    for (uint8_t i = 0; i < def.option_count; ++i) {
        if (strcmp(def.options[i], value) == 0) return i;
    }
    if (def.wire_options) {
        for (uint8_t i = 0; i < def.option_count; ++i) {
            if (strcmp(def.wire_options[i], value) == 0) return i;
        }
    }
    return -1;
}

const char *option_value_at(const As11SettingDef &def, int index) {
    if (index < 0 || index >= def.option_count) return nullptr;
    return def.options[index];
}

const char *option_wire_value_at(const As11SettingDef &def, int index) {
    if (index < 0 || index >= def.option_count) return nullptr;
    return def.wire_options ? def.wire_options[index] : def.options[index];
}

const char *known_var_alias(const char *name) {
    if (!name) return nullptr;
    for (size_t i = 0;
         i < sizeof(KNOWN_VAR_ALIASES) / sizeof(KNOWN_VAR_ALIASES[0]);
         ++i) {
        if (strcmp(KNOWN_VAR_ALIASES[i].name, name) == 0) {
            return KNOWN_VAR_ALIASES[i].alias;
        }
    }
    return nullptr;
}

const char *profile_name_for_mode(int mode) {
    static const char *const names[] = {
        "CpapProfile",
        "AutoSetProfile",
        "AutoSetForHerProfile",
        "SpontProfile",
        "STProfile",
        "TimedProfile",
        "VAutoProfile",
        "ASVProfile",
        "ASVAutoProfile",
        "iVAPSProfile",
        "PACProfile",
    };
    if (mode < 0 || mode >= static_cast<int>(sizeof(names) / sizeof(names[0]))) {
        return nullptr;
    }
    return names[mode];
}

uint16_t profile_modes_from_result(JsonObjectConst result) {
    uint16_t mask = 0;
    JsonObjectConst profiles =
        result["TherapyProfiles"].as<JsonObjectConst>();
    if (profiles.isNull()) return 0;
    for (int mode = 0; mode <= 10; ++mode) {
        const char *profile_name = profile_name_for_mode(mode);
        if (!profile_name) continue;
        JsonObjectConst profile =
            profiles[profile_name].as<JsonObjectConst>();
        if (!profile.isNull()) {
            mask |= MODE_BIT(mode);
        }
    }
    return mask;
}

JsonObjectConst profile_from_result(JsonObjectConst result, int mode) {
    const char *profile_name = profile_name_for_mode(mode);
    if (!profile_name) return JsonObjectConst();

    JsonObjectConst profile = result[profile_name].as<JsonObjectConst>();
    if (!profile.isNull()) return profile;
    return result["TherapyProfiles"][profile_name].as<JsonObjectConst>();
}

const char *profile_prefix_for_mode(int mode) {
    static const char *const prefixes[] = {
        "Cpap", "AutoSet", "HerAuto", "Spont", "ST", "Timed",
        "VAuto", "ASV", "ASVAuto", "iVAPS", "PAC",
    };
    if (mode < 0 ||
        mode >= static_cast<int>(sizeof(prefixes) / sizeof(prefixes[0]))) {
        return nullptr;
    }
    return prefixes[mode];
}

const char *setting_write_key(const As11SettingDef &def,
                              int mode,
                              char *buffer,
                              size_t buffer_len) {
    if (strcmp(def.name, "TherapyMode") == 0) {
        return known_var_alias("ActiveTherapyProfile");
    }
    if (def.profile_field) {
        const char *prefix = profile_prefix_for_mode(mode);
        if (!prefix || !buffer || !buffer_len) return nullptr;
        const int written = snprintf(buffer, buffer_len, "%s-%s",
                                     prefix, def.profile_field);
        if (written < 0 || static_cast<size_t>(written) >= buffer_len) {
            return nullptr;
        }
        return known_var_alias(buffer);
    }
    if (def.feature_field) {
        return known_var_alias(def.feature_field);
    }
    return nullptr;
}

JsonVariantConst value_for_setting(JsonObjectConst object,
                                   const As11SettingDef &def) {
    JsonVariantConst value = object[def.name];
    if (!value.isNull()) return value;
    return def.flat_name ? object[def.flat_name] : JsonVariantConst();
}

JsonObjectConst feature_object_from_result(JsonObjectConst result,
                                           const As11SettingDef &def) {
    if (!def.feature_name) return JsonObjectConst();
    return result["FeatureProfiles"][def.feature_name].as<JsonObjectConst>();
}

bool cpap_like_mode(int mode) {
    return mode == 0 || mode == 1 || mode == 2;
}

bool feature_available_for_mode(const char *feature_name,
                                int mode,
                                bool present) {
    if (!feature_name || !present) return false;
    if (strcmp(feature_name, "ComfortFeature") == 0) return mode == 1;
    if (strcmp(feature_name, "EprFeature") == 0) return cpap_like_mode(mode);
    return true;
}

int enum_index_from_text(const As11SettingDef &def, const char *text) {
    int index = option_index_of(def, text);
    if (index >= 0) return index;

    int parsed = -1;
    if (text && parse_int_text(text, parsed)) return parsed;

    if (strcmp(def.name, "TherapyMode") != 0) return -1;
    const std::string wanted = compact_mode_key(text);
    if (wanted.empty()) return -1;
    for (uint8_t i = 0; i < def.option_count; ++i) {
        if (compact_mode_key(def.options[i]) == wanted) return i;
    }
    return -1;
}

int mode_index_from_json(JsonVariantConst value) {
    if (value.isNull()) return -1;
    if (value.is<int>()) return value.as<int>();
    if (value.is<long>()) return static_cast<int>(value.as<long>());
    if (value.is<const char *>()) {
        return as11_mode_index_from_value(value.as<const char *>());
    }
    if (value.is<std::string>()) {
        return as11_mode_index_from_value(value.as<std::string>());
    }
    return as11_mode_index_from_value(value_to_string(value));
}

std::string normalize_value_for_def(const As11SettingDef &def,
                                    JsonVariantConst value) {
    if (def.kind == As11SettingKind::Number && def.scale_div > 1) {
        double numeric = 0;
        bool parsed = false;
        if (value.is<const char *>()) {
            parsed = parse_iso_seconds(value.as<const char *>(), numeric);
            if (!parsed) parsed = parse_number(value.as<const char *>(), numeric);
        } else if (json_is_number(value)) {
            numeric = value.as<double>();
            parsed = true;
        }
        if (parsed) {
            return std::to_string(lround(numeric * def.scale_div));
        }
    }

    if (def.kind == As11SettingKind::Enum) {
        int index = -1;
        if (value.is<int>()) {
            index = value.as<int>();
        } else if (value.is<long>()) {
            index = static_cast<int>(value.as<long>());
        } else if (value.is<const char *>()) {
            index = enum_index_from_text(def, value.as<const char *>());
        }
        if (index >= 0 && index < def.option_count) {
            return std::to_string(index);
        }
    }
    return value_to_string(value);
}

bool setting_value_matches(const As11SettingDef &def,
                           const std::string &confirmed,
                           const std::string &pending) {
    if (def.kind == As11SettingKind::Number) {
        double a = 0;
        double b = 0;
        if (!parse_number(confirmed, a) || !parse_number(pending, b)) {
            return confirmed == pending;
        }
        const double tolerance = def.step > 0 ? def.step / 20.0 : 0.001;
        return fabs(a - b) <= tolerance;
    }

    if (def.kind == As11SettingKind::Bool) {
        bool a = false;
        bool b = false;
        if (parse_bool_yesno(confirmed, a) && parse_bool_yesno(pending, b)) {
            return a == b;
        }
    }

    return confirmed == pending;
}

bool json_literal_for_set(const As11SettingDef &def,
                          JsonVariantConst value,
                          std::string &out) {
    if (strcmp(def.name, "TherapyMode") == 0) {
        int index = mode_index_from_json(value);
        const char *profile = profile_name_for_mode(index);
        if (!profile) return false;
        out = "\"";
        out += profile;
        out += "\"";
        return true;
    }

    if (def.kind == As11SettingKind::Number) {
        if (!json_is_number(value)) return false;
        const double numeric = value.as<double>();
        if (numeric < def.min_value || numeric > def.max_value) return false;
        if (strcmp(def.name, "SetMinInspiratoryTime") == 0 ||
            strcmp(def.name, "SetMaxInspiratoryTime") == 0 ||
            strcmp(def.name, "SetInspiratoryTime") == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "\"PT%gS\"", numeric);
            out = buf;
        } else {
            out = value_to_string(value);
        }
        return !out.empty();
    }

    if (def.kind == As11SettingKind::Enum) {
        int index = -1;
        if (value.is<int>()) {
            index = value.as<int>();
        } else if (value.is<long>()) {
            index = static_cast<int>(value.as<long>());
        } else if (value.is<const char *>()) {
            index = enum_index_from_text(def, value.as<const char *>());
        }
        const char *wire_value = option_wire_value_at(def, index);
        if (!wire_value) return false;
        out = "\"";
        out += json_escape(wire_value);
        out += "\"";
        return true;
    }

    if (value.is<bool>()) {
        if (def.kind != As11SettingKind::Bool) return false;
        out = value.as<bool>() ? "true" : "false";
        return true;
    }

    if (def.kind == As11SettingKind::Text && value.is<const char *>()) {
        out = "\"";
        out += json_escape(value.as<const char *>());
        out += "\"";
        return true;
    }
    return false;
}

}  // namespace

As11StoredValue::As11StoredValue(const As11StoredValue &other) {
    set(other.str());
}

As11StoredValue::As11StoredValue(As11StoredValue &&other) noexcept {
    if (other.heap_) {
        heap_ = other.heap_;
        length_ = other.length_;
        other.heap_ = nullptr;
        other.length_ = 0;
        other.inline_[0] = 0;
        return;
    }
    if (other.length_) {
        memcpy(inline_, other.inline_, other.length_ + 1);
        length_ = other.length_;
        other.clear();
    }
}

As11StoredValue::~As11StoredValue() {
    clear();
}

As11StoredValue &As11StoredValue::operator=(const As11StoredValue &other) {
    if (this != &other) set(other.str());
    return *this;
}

As11StoredValue &As11StoredValue::operator=(
    As11StoredValue &&other) noexcept {
    if (this == &other) return *this;
    clear();
    if (other.heap_) {
        heap_ = other.heap_;
        length_ = other.length_;
        other.heap_ = nullptr;
        other.length_ = 0;
        other.inline_[0] = 0;
        return *this;
    }
    if (other.length_) {
        memcpy(inline_, other.inline_, other.length_ + 1);
        length_ = other.length_;
        other.clear();
    }
    return *this;
}

bool As11StoredValue::set(const std::string &value) {
    const size_t len = value.size();
    if (len <= InlineCapacity) {
        settings_free(heap_);
        heap_ = nullptr;
        length_ = len;
        if (len) memcpy(inline_, value.data(), len);
        inline_[len] = 0;
        return true;
    }

    char *next = static_cast<char *>(settings_alloc_large(len + 1));
    if (!next) return false;
    memcpy(next, value.data(), len);
    next[len] = 0;
    settings_free(heap_);
    heap_ = next;
    length_ = len;
    inline_[0] = 0;
    return true;
}

void As11StoredValue::clear() {
    settings_free(heap_);
    heap_ = nullptr;
    length_ = 0;
    inline_[0] = 0;
}

bool As11SettingsState::apply_settings_get_response(
    const std::string &payload,
    uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) {
        result = doc["error"]["data"].as<JsonObjectConst>();
    }
    if (result.isNull()) return false;

    int fallback_mode = mode_index();
    JsonVariantConst active = result["_MOP"];
    if (!active.isNull()) {
        fallback_mode = as11_mode_index_from_value(value_to_string(active));
    }
    const uint16_t profile_modes = profile_modes_from_result(result);
    if (profile_modes) {
        supported_mode_mask_ = profile_modes;
        clear_profile_values();
    }

    bool any = false;
    auto remember_value = [&](size_t index, const std::string &normalized) {
        values_[index] = normalized;
        if (pending_[index] &&
            setting_value_matches(SETTINGS[index],
                                  values_[index],
                                  pending_values_[index])) {
            clear_pending(index);
            last_write_status_ = pending_count_ ? "waiting_readback"
                                                : "confirmed";
            last_write_ms_ = now_ms;
        }
        any = true;
    };
    auto remember_profile_value = [&](int mode,
                                      size_t index,
                                      const std::string &normalized) {
        set_profile_value(mode, index, normalized);
        if (mode == fallback_mode) values_[index] = normalized;
        if (pending_[index] &&
            setting_value_matches(SETTINGS[index],
                                  normalized,
                                  pending_values_[index])) {
            clear_pending(index);
            last_write_status_ = pending_count_ ? "waiting_readback"
                                                : "confirmed";
            last_write_ms_ = now_ms;
        }
        any = true;
    };

    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        JsonVariantConst value;
        if (SETTINGS[i].feature_name) {
            JsonObjectConst feature = feature_object_from_result(result,
                                                                 SETTINGS[i]);
            if (feature.isNull()) continue;
            feature_present_[i] = true;
            value = feature[SETTINGS[i].feature_field];
            if (value.isNull()) {
                values_[i] = "";
                continue;
            }
        } else {
            value = value_for_setting(result, SETTINGS[i]);
            if (value.isNull()) continue;
        }
        remember_value(i, normalize_value_for_def(SETTINGS[i], value));
    }

    for (int mode = 0; mode < static_cast<int>(MaxModes); ++mode) {
        JsonObjectConst profile = profile_from_result(result, mode);
        if (profile.isNull()) continue;

        for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
            if (!SETTINGS[i].profile_field) continue;
            if (!as11_setting_visible_for_mode(SETTINGS[i], mode)) continue;
            JsonVariantConst value = profile[SETTINGS[i].profile_field];
            if (value.isNull()) continue;
            remember_profile_value(
                mode, i, normalize_value_for_def(SETTINGS[i], value));
        }
    }

    if (any) {
        valid_ = true;
        updated_ms_ = now_ms;
    }
    return any;
}

bool As11SettingsState::note_set_request(const std::string &params_json,
                                         uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, params_json);
    if (err || !doc.is<JsonObjectConst>()) return false;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    int mode = mode_index();
    int target_mode = mode_index_from_json(root["TherapyMode"]);
    if (target_mode < 0) {
        target_mode = mode_index_from_json(root["_MOP"]);
    }
    if (target_mode >= 0) mode = target_mode;

    bool any = false;
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (!as11_setting_visible_for_mode(SETTINGS[i], mode)) continue;
        JsonVariantConst value = value_for_setting(root, SETTINGS[i]);
        if (value.isNull()) {
            char key[80];
            const char *write_key = setting_write_key(SETTINGS[i],
                                                      mode,
                                                      key,
                                                      sizeof(key));
            if (write_key) value = root[write_key];
        }
        if (value.isNull()) continue;
        std::string pending_value = normalize_value_for_def(SETTINGS[i], value);
        if (pending_value.empty()) continue;
        if (!pending_[i]) pending_count_++;
        pending_[i] = true;
        pending_values_[i] = pending_value;
        pending_since_ms_[i] = now_ms;
        any = true;
    }

    if (any) {
        last_write_status_ = "sent";
        last_write_ms_ = now_ms;
    }
    return any;
}

void As11SettingsState::note_set_response(bool is_error, uint32_t now_ms) {
    if (!pending_count_) return;
    if (is_error) {
        clear_all_pending();
        last_write_status_ = "set_error";
    } else {
        last_write_status_ = "waiting_readback";
    }
    last_write_ms_ = now_ms;
}

void As11SettingsState::note_set_cancelled(const char *reason,
                                           uint32_t now_ms) {
    if (!pending_count_) return;
    clear_all_pending();
    last_write_status_ = reason ? reason : "cancelled";
    last_write_ms_ = now_ms;
}

void As11SettingsState::clear() {
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        values_[i] = "";
        feature_present_[i] = false;
        pending_values_[i] = "";
        pending_since_ms_[i] = 0;
        pending_[i] = false;
    }
    clear_profile_values();
    pending_count_ = 0;
    last_write_status_ = "";
    last_write_ms_ = 0;
    valid_ = false;
    updated_ms_ = 0;
    supported_mode_mask_ = 0;
}

void As11SettingsState::clear_pending(size_t index) {
    if (index >= SETTINGS_COUNT || !pending_[index]) return;
    pending_[index] = false;
    pending_values_[index] = "";
    pending_since_ms_[index] = 0;
    if (pending_count_) pending_count_--;
}

void As11SettingsState::clear_all_pending() {
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) clear_pending(i);
}

void As11SettingsState::clear_profile_values() {
    for (ProfileValueSlot &slot : profile_values_) {
        slot.used = false;
        slot.mode = 0;
        slot.index = 0;
        slot.value.clear();
    }
}

const As11StoredValue *As11SettingsState::profile_value(
    size_t index,
    int mode) const {
    if (index >= SETTINGS_COUNT || mode < 0 ||
        mode >= static_cast<int>(MaxModes)) {
        return nullptr;
    }
    for (const ProfileValueSlot &slot : profile_values_) {
        if (!slot.used) continue;
        if (slot.mode == static_cast<uint8_t>(mode) &&
            slot.index == static_cast<uint8_t>(index)) {
            return &slot.value;
        }
    }
    return nullptr;
}

bool As11SettingsState::set_profile_value(int mode,
                                          size_t index,
                                          const std::string &value) {
    if (mode < 0 || mode >= static_cast<int>(MaxModes) ||
        index >= SETTINGS_COUNT) {
        return false;
    }

    for (ProfileValueSlot &slot : profile_values_) {
        if (!slot.used) continue;
        if (slot.mode == static_cast<uint8_t>(mode) &&
            slot.index == static_cast<uint8_t>(index)) {
            return slot.value.set(value);
        }
    }

    for (ProfileValueSlot &slot : profile_values_) {
        if (slot.used) continue;
        slot.mode = static_cast<uint8_t>(mode);
        slot.index = static_cast<uint8_t>(index);
        if (!slot.value.set(value)) {
            slot.mode = 0;
            slot.index = 0;
            return false;
        }
        slot.used = true;
        return true;
    }

    return false;
}

std::string As11SettingsState::value(size_t index, int mode) const {
    if (index >= SETTINGS_COUNT) return "";
    if (mode >= 0 && mode < static_cast<int>(MaxModes)) {
        if (strcmp(SETTINGS[index].name, "TherapyMode") == 0) {
            return std::to_string(mode);
        }
        const As11StoredValue *stored = profile_value(index, mode);
        if (SETTINGS[index].profile_field && stored && !stored->empty()) {
            return stored->str();
        }
    }
    return values_[index];
}

bool As11SettingsState::feature_present(size_t index) const {
    if (index >= SETTINGS_COUNT) return false;
    return feature_present_[index];
}

bool As11SettingsState::setting_visible(size_t index, int mode) const {
    if (index >= SETTINGS_COUNT) return false;
    const As11SettingDef &def = SETTINGS[index];
    if (!as11_setting_visible_for_mode(def, mode)) return false;
    if (!def.feature_name) return true;
    return feature_available_for_mode(def.feature_name,
                                      mode,
                                      feature_present_[index]);
}

int As11SettingsState::mode_index() const {
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (strcmp(SETTINGS[i].name, "TherapyMode") == 0) {
            return as11_mode_index_from_value(values_[i]);
        }
    }
    return -1;
}

size_t as11_setting_count() {
    return SETTINGS_COUNT;
}

const As11SettingDef &as11_setting(size_t index) {
    return SETTINGS[index];
}

const As11SettingDef *as11_find_setting(const char *name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (strcmp(SETTINGS[i].name, name) == 0 ||
            (SETTINGS[i].flat_name && strcmp(SETTINGS[i].flat_name, name) == 0)) {
            return &SETTINGS[i];
        }
    }
    return nullptr;
}

bool as11_setting_visible_for_mode(const As11SettingDef &def, int mode) {
    if (mode < 0 || mode > 10) return strcmp(def.name, "TherapyMode") == 0;
    return (def.mode_mask & MODE_BIT(mode)) != 0;
}

bool as11_setting_option_supported(const As11SettingDef &def,
                                   uint8_t option_index,
                                   uint16_t supported_mode_mask) {
    if (strcmp(def.name, "TherapyMode") != 0) return true;
    if (option_index >= def.option_count) return false;
    return supported_mode_mask &&
           (supported_mode_mask & (1u << option_index)) != 0;
}

bool as11_setting_readable_via_rpc(const As11SettingDef &def) {
    return def.flat_name != nullptr || def.profile_field != nullptr ||
           (def.feature_name != nullptr && def.feature_field != nullptr);
}

bool as11_setting_writable_via_rpc(const As11SettingDef &def, int mode) {
    char key[80];
    return setting_write_key(def, mode, key, sizeof(key)) != nullptr;
}

int as11_mode_index_from_value(const std::string &value) {
    int parsed = -1;
    if (parse_int_text(value, parsed) && parsed >= 0 && parsed <= 10) {
        return parsed;
    }
    const As11SettingDef *mode_def = as11_find_setting("TherapyMode");
    if (!mode_def) return -1;
    const int index = enum_index_from_text(*mode_def, value.c_str());
    return index >= 0 && index <= 10 ? index : -1;
}

const char *as11_mode_name(int mode) {
    static const char *const names[] = {
        "CPAP", "AutoSet", "AutoSet For Her", "S", "ST", "T",
        "VAuto", "ASV", "ASVAuto", "iVAPS", "PAC",
    };
    if (mode < 0 || mode >= static_cast<int>(sizeof(names) / sizeof(names[0]))) {
        return "";
    }
    return names[mode];
}

std::string as11_setting_display_value(const As11SettingDef &def,
                                       const std::string &raw) {
    if (raw.empty()) return "";
    if (def.kind == As11SettingKind::Enum) {
        int index = -1;
        if (parse_int_text(raw, index)) {
            const char *label = option_value_at(def, index);
            if (label) return label;
        }
        return raw;
    }
    if (def.kind == As11SettingKind::Number && def.scale_div > 1) {
        double value = 0;
        if (!parse_number(raw, value)) return raw;
        value /= def.scale_div;
        char buf[24];
        snprintf(buf, sizeof(buf), "%.*f", def.decimals, value);
        return buf;
    }
    return raw;
}

std::string as11_settings_get_params_json(int mode) {
    (void)mode;
    std::string out = "[";
    out += "\"_MOP\",\"TherapyProfiles\",\"FeatureProfiles\"";
    out += "]";
    return out;
}

std::string as11_build_set_params_from_json(const std::string &body,
                                            int mode,
                                            size_t &accepted) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.is<JsonObjectConst>()) {
        accepted = 0;
        return "{}";
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();
    int target_mode = mode_index_from_json(root["TherapyMode"]);
    if (target_mode >= 0) mode = target_mode;

    std::string out = "{";
    accepted = 0;
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (!as11_setting_visible_for_mode(SETTINGS[i], mode)) continue;
        JsonVariantConst value = root[SETTINGS[i].name];
        if (value.isNull()) continue;
        char key[80];
        const char *write_key = setting_write_key(SETTINGS[i],
                                                  mode,
                                                  key,
                                                  sizeof(key));
        if (!write_key) continue;
        std::string literal;
        if (!json_literal_for_set(SETTINGS[i], value, literal)) continue;
        if (accepted) out += ",";
        out += "\"";
        out += write_key;
        out += "\":";
        out += literal;
        accepted++;
    }
    out += "}";
    return out;
}

}  // namespace aircannect
