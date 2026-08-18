
const char *const THERAPY_MODE_OPTIONS[] = {
    "CPAP", "AutoSet", "AutoSet For Her", "S", "ST", "T", "VAuto", "ASV", "ASVAuto", "iVAPS", "PAC",
};
const char *const THERAPY_MODE_WIRE_OPTIONS[] = {
    "CpapProfile", "AutoSetProfile", "AutoSetForHerProfile", "SpontProfile", "STProfile", "TimedProfile", "VAutoProfile", "ASVProfile", "ASVAutoProfile", "iVAPSProfile", "PACProfile",
};

const char *const SENSITIVITY_OPTIONS[] = {
    "Very Low", "Low", "Medium", "High", "Very High", "Very High 1", "Very High 2",
};
const char *const SENSITIVITY_WIRE_OPTIONS[] = {
    "VeryLow", "Low", "Medium", "High", "VeryHigh", "VeryHigh1", "VeryHigh2",
};

const char *const OFF_FIXED_OPTIONS[] = {
    "Off", "Fixed",
};

const char *const OFF_ON_OPTIONS[] = {
    "Off", "On",
};

const char *const MIN_ON_OPTIONS[] = {
    "Min", "On",
};

const char *const HEIGHT_UNIT_OPTIONS[] = {
    "cm", "inch",
};

const char *const EPR_TYPE_OPTIONS[] = {
    "Ramp", "FullTime",
};

const char *const OFF_ON_AUTO_OPTIONS[] = {
    "Off", "On", "Auto",
};

const char *const MASK_TYPE_OPTIONS[] = {
    "Pillows", "FullFace", "Nasal", "Paediatric",
};

const char *const TUBE_TYPE_OPTIONS[] = {
    "15mm Non-Heated", "19mm Non-Heated", "3m Non-Heated",
};
const char *const TUBE_TYPE_WIRE_OPTIONS[] = {
    "15mmNonHeated", "19mmNonHeated", "3mNonHeated",
};

const char *const YES_NO_WIRE_ORDER_OPTIONS[] = {
    "Yes", "No",
};

const char *const AUTO_MANUAL_OPTIONS[] = {
    "Auto", "Manual",
};

const char *const NO_YES_WIRE_ORDER_OPTIONS[] = {
    "No", "Yes",
};

const char *const LANGUAGE_OPTIONS[] = {
    "English", "French", "German", "Italian", "Spanish EU", "Spanish US", "Portuguese EU", "Portuguese US", "Dutch", "Swedish", "Danish", "Norwegian", "Finnish", "Russian", "Turkish", "Polish", "Czech", "Greek", "Estonian", "Chinese Traditional", "Chinese Simplified", "Japanese", "Korean", "Croatian", "Hungarian", "Romanian", "Slovenian",
};
const char *const LANGUAGE_WIRE_OPTIONS[] = {
    "English", "French", "German", "Italian", "SpanishEU", "SpanishUS", "PortugueseEU", "PortugueseUS", "Dutch", "Swedish", "Danish", "Norwegian", "Finnish", "Russian", "Turkish", "Polish", "Czech", "Greek", "Estonian", "ChineseTraditional", "ChineseSimplified", "Japanese", "Korean", "Croatian", "Hungarian", "Romanian", "Slovenian",
};

const char *const NO_YES_NOT_ANSWERED_OPTIONS[] = {
    "No", "Yes", "NotAnswered",
};

const char *const TEMPERATURE_UNIT_OPTIONS[] = {
    "Celsius", "Fahrenheit",
};

const char *const PATIENT_VIEW_OPTIONS[] = {
    "Basic", "Full",
};

const char *const REMINDER_FREQUENCY_OPTIONS[] = {
    "Daily", "Weekly", "Fortnightly", "Off",
};

const char *const MONTH_PERIOD_OPTIONS[] = {
    "Off", "1 month", "2 months", "3 months", "4 months", "5 months", "6 months", "7 months", "8 months", "9 months", "10 months", "11 months", "12 months",
};
const char *const MONTH_PERIOD_WIRE_OPTIONS[] = {
    "P0M", "P1M", "P2M", "P3M", "P4M", "P5M", "P6M", "P7M", "P8M", "P9M", "P10M", "P11M", "P12M",
};

const char *const SPLASH_SCREEN_OPTIONS[] = {
    "Off", "ResMed",
};

const char *const STRING_PERCENTAGE_OPTIONS[] = {
    "String", "Percentage",
};

const As11SettingCompositeOption RAMP_TIME_COMPOSITE_OPTIONS[] = {
    {"Off", 0, nullptr},
    {"Auto", 2, nullptr},
    {"5", 1, "5"},
    {"10", 1, "10"},
    {"15", 1, "15"},
    {"20", 1, "20"},
    {"25", 1, "25"},
    {"30", 1, "30"},
    {"35", 1, "35"},
    {"40", 1, "40"},
    {"45", 1, "45"},
};

const As11SettingCompositeOption HUMIDITY_LEVEL_COMPOSITE_OPTIONS[] = {
    {"Off", 0, nullptr},
    {"1", 1, "1"},
    {"2", 1, "2"},
    {"3", 1, "3"},
    {"4", 1, "4"},
    {"5", 1, "5"},
    {"6", 1, "6"},
    {"7", 1, "7"},
    {"8", 1, "8"},
};

const As11SettingCompositeOption RISE_TIME_COMPOSITE_OPTIONS[] = {
    {"Min", 0, nullptr},
    {"150", 1, "150"},
    {"200", 1, "200"},
    {"250", 1, "250"},
    {"300", 1, "300"},
    {"350", 1, "350"},
    {"400", 1, "400"},
    {"450", 1, "450"},
    {"500", 1, "500"},
    {"550", 1, "550"},
    {"600", 1, "600"},
    {"650", 1, "650"},
    {"700", 1, "700"},
    {"750", 1, "750"},
    {"800", 1, "800"},
    {"850", 1, "850"},
    {"900", 1, "900"},
};

const As11SettingCompositeOption FALL_TIME_COMPOSITE_OPTIONS[] = {
    {"Min", 0, nullptr},
    {"100", 1, "100"},
    {"200", 1, "200"},
    {"300", 1, "300"},
    {"400", 1, "400"},
    {"500", 1, "500"},
    {"600", 1, "600"},
    {"700", 1, "700"},
};

const As11SettingDef SETTINGS[] = {
    flat_setting(
        "MOP", "TherapyMode", "Mode",
        "Therapy", "therapy", 0x07FFu,
        ranged_enum_value(THERAPY_MODE_OPTIONS, THERAPY_MODE_WIRE_OPTIONS, 0.0f, 10.0f, 1.0f)),
    therapy_setting(
        "MPA", As11ProfileId::AutoSet, "MaxPressure",
        "Max Pressure", "Therapy", "pressure", 0x0002u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "MPI", As11ProfileId::AutoSet, "MinPressure",
        "Min Pressure", "Therapy", "pressure", 0x0002u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "STU", As11ProfileId::AutoSet, "StartPressure",
        "Start Pressure", "Comfort", "therapy_start_pressure", 0x0002u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "HMA", As11ProfileId::HerAuto, "MaxPressure",
        "Max Pressure", "Therapy", "pressure", 0x0004u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "HMI", As11ProfileId::HerAuto, "MinPressure",
        "Min Pressure", "Therapy", "pressure", 0x0004u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "HSP", As11ProfileId::HerAuto, "StartPressure",
        "Start Pressure", "Comfort", "therapy_start_pressure", 0x0004u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "IPC", As11ProfileId::Cpap, "SetPressure",
        "Set Pressure", "Therapy", "pressure", 0x0001u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "STP", As11ProfileId::Cpap, "StartPressure",
        "Start Pressure", "Comfort", "therapy_start_pressure", 0x0001u,
        number_value(4.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "C11", As11ProfileId::Cpap, "TriggerSensitivity",
        "Trigger", "Therapy", "trigger_cycle", 0x0001u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "ZZ1", As11ProfileId::Spont, "TargetInspiratoryPressure",
        "IPAP", "Therapy", "pressure", 0x0008u,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "ZZ2", As11ProfileId::Spont, "TargetExpiratoryPressure",
        "EPAP", "Therapy", "pressure", 0x0008u,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "ZZ3", As11ProfileId::Spont, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", 0x0008u,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "ZZ5", As11ProfileId::Spont, "RespiratoryRateEnable",
        "Backup Rate", "Therapy", "backup_rate", 0x0008u,
        enum_value(OFF_FIXED_OPTIONS)),
    therapy_setting(
        "ZZ8", As11ProfileId::Spont, "SetMinInspiratoryTime",
        "Ti Min", "Therapy", "timing", 0x0008u,
        number_value(0.1f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "ZZ7", As11ProfileId::Spont, "SetMaxInspiratoryTime",
        "Ti Max", "Therapy", "timing", 0x0008u,
        number_value(0.3f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "ZZ4", As11ProfileId::Spont, "EasyBreatheEnable",
        "Easy-Breathe", "Therapy", "comfort", 0x0008u,
        enum_value(OFF_ON_OPTIONS)),
    therapy_setting(
        "ZZ9", As11ProfileId::Spont, "RiseTimeEnable",
        "Rise Time Mode", "Therapy", "timing", 0x0008u,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "Z10", As11ProfileId::Spont, "RiseTime",
        "Rise Time", "Therapy", "timing", 0x0008u,
        number_value(0.15f, 0.9f, 0.05f, 1000, 2)),
    therapy_setting(
        "Z16", As11ProfileId::Spont, "FallTimeEnable",
        "Fall Time Mode", "Therapy", "timing", 0x0008u,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "Z17", As11ProfileId::Spont, "FallTime",
        "Fall Time", "Therapy", "timing", 0x0008u,
        number_value(0.1f, 0.7f, 0.1f, 1000, 1)),
    therapy_setting(
        "Z11", As11ProfileId::Spont, "TriggerSensitivity",
        "Trigger", "Therapy", "trigger_cycle", 0x0008u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "Z12", As11ProfileId::Spont, "CycleSensitivity",
        "Cycle", "Therapy", "trigger_cycle", 0x0008u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "XC1", As11ProfileId::ASV, "TargetExpiratoryPressure",
        "EPAP", "Therapy", "pressure", 0x0080u,
        number_value(4.0f, 15.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XC3", As11ProfileId::ASV, "MinPressureSupport",
        "Min PS", "Therapy", "pressure_support", 0x0080u,
        number_value(0.0f, 6.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XC2", As11ProfileId::ASV, "MaxPressureSupport",
        "Max PS", "Therapy", "pressure_support", 0x0080u,
        number_value(5.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XC0", As11ProfileId::ASV, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", 0x0080u,
        number_value(4.0f, 15.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XD2", As11ProfileId::ASVAuto, "MinExpiratoryPressure",
        "Min EPAP", "Therapy", "pressure", 0x0100u,
        number_value(4.0f, 15.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XD1", As11ProfileId::ASVAuto, "MaxExpiratoryPressure",
        "Max EPAP", "Therapy", "pressure", 0x0100u,
        number_value(4.0f, 15.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XD0", As11ProfileId::ASVAuto, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", 0x0100u,
        number_value(4.0f, 15.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XD4", As11ProfileId::ASVAuto, "MinPressureSupport",
        "Min PS", "Therapy", "pressure_support", 0x0100u,
        number_value(0.0f, 6.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XD3", As11ProfileId::ASVAuto, "MaxPressureSupport",
        "Max PS", "Therapy", "pressure_support", 0x0100u,
        number_value(5.0f, 20.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XE1", As11ProfileId::VAuto, "MaxInspiratoryPressure",
        "Max IPAP", "Therapy", "pressure", 0x0040u,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XE2", As11ProfileId::VAuto, "MinExpiratoryPressure",
        "Min EPAP", "Therapy", "pressure", 0x0040u,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XE0", As11ProfileId::VAuto, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", 0x0040u,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XE5", As11ProfileId::VAuto, "SetMinInspiratoryTime",
        "Ti Min", "Therapy", "timing", 0x0040u,
        number_value(0.1f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "XE4", As11ProfileId::VAuto, "SetMaxInspiratoryTime",
        "Ti Max", "Therapy", "timing", 0x0040u,
        number_value(0.3f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "XE3", As11ProfileId::VAuto, "SetPressureSupport",
        "PS", "Therapy", "pressure_support", 0x0040u,
        number_value(0.0f, 10.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XE6", As11ProfileId::VAuto, "TriggerSensitivity",
        "Trigger", "Therapy", "trigger_cycle", 0x0040u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "XE7", As11ProfileId::VAuto, "CycleSensitivity",
        "Cycle", "Therapy", "trigger_cycle", 0x0040u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "XA1", As11ProfileId::ST, "TargetInspiratoryPressure",
        "IPAP", "Therapy", "pressure", 0x0010u,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XA2", As11ProfileId::ST, "TargetExpiratoryPressure",
        "EPAP", "Therapy", "pressure", 0x0010u,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XA3", As11ProfileId::ST, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", 0x0010u,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XAC", As11ProfileId::ST, "IntelligentBackupRateEnable",
        "iBR", "Therapy", "backup_rate", 0x0010u,
        enum_value(OFF_ON_OPTIONS)),
    therapy_setting(
        "XAD", As11ProfileId::ST, "TargetRespiratoryRate",
        "Target Pt Rate", "Therapy", "backup_rate", 0x0010u,
        number_value(0.0f, 50.0f, 1.0f, 5, 0)),
    therapy_setting(
        "XA6", As11ProfileId::ST, "SetRespiratoryRate",
        "Respiratory Rate", "Therapy", "backup_rate", 0x0010u,
        number_value(0.0f, 50.0f, 1.0f, 5, 1)),
    therapy_setting(
        "XA8", As11ProfileId::ST, "SetMinInspiratoryTime",
        "Ti Min", "Therapy", "timing", 0x0010u,
        number_value(0.1f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "XA7", As11ProfileId::ST, "SetMaxInspiratoryTime",
        "Ti Max", "Therapy", "timing", 0x0010u,
        number_value(0.3f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "XA9", As11ProfileId::ST, "RiseTimeEnable",
        "Rise Time Mode", "Therapy", "timing", 0x0010u,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "XAA", As11ProfileId::ST, "RiseTime",
        "Rise Time", "Therapy", "timing", 0x0010u,
        number_value(0.15f, 0.9f, 0.05f, 1000, 2)),
    therapy_setting(
        "XAM", As11ProfileId::ST, "FallTimeEnable",
        "Fall Time Mode", "Therapy", "timing", 0x0010u,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "XAP", As11ProfileId::ST, "FallTime",
        "Fall Time", "Therapy", "timing", 0x0010u,
        number_value(0.1f, 0.7f, 0.1f, 1000, 1)),
    therapy_setting(
        "ZU1", As11ProfileId::ST, "TriggerSensitivity",
        "Trigger", "Therapy", "trigger_cycle", 0x0010u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "XAB", As11ProfileId::ST, "CycleSensitivity",
        "Cycle", "Therapy", "trigger_cycle", 0x0010u,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "XB1", As11ProfileId::Timed, "TargetInspiratoryPressure",
        "IPAP", "Therapy", "pressure", 0x0020u,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XB2", As11ProfileId::Timed, "TargetExpiratoryPressure",
        "EPAP", "Therapy", "pressure", 0x0020u,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XB0", As11ProfileId::Timed, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", 0x0020u,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "XB4", As11ProfileId::Timed, "SetRespiratoryRate",
        "Respiratory Rate", "Therapy", "backup_rate", 0x0020u,
        number_value(5.0f, 50.0f, 1.0f, 5, 1)),
    therapy_setting(
        "XB5", As11ProfileId::Timed, "SetInspiratoryTime",
        "Ti", "Therapy", "timing", 0x0020u,
        number_value(0.3f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "XB6", As11ProfileId::Timed, "RiseTimeEnable",
        "Rise Time Mode", "Therapy", "timing", 0x0020u,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "XB7", As11ProfileId::Timed, "RiseTime",
        "Rise Time", "Therapy", "timing", 0x0020u,
        number_value(0.15f, 0.9f, 0.05f, 1000, 2)),
    therapy_setting(
        "PHT", As11ProfileId::iVAPS, "PatientHeight",
        "Height", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(110.0f, 250.0f, 5.0f, 1, 0)),
    flat_setting(
        "PHI", nullptr, "Height",
        "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(44.0f, 100.0f, 2.0f, 1, 0)),
    therapy_setting(
        "IBR", As11ProfileId::iVAPS, "TargetRespiratoryRate",
        "Target Pt Rate", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(0.0f, 50.0f, 1.0f, 5, 0)),
    therapy_setting(
        "ITV", As11ProfileId::iVAPS, "TargetAlveolarVentilation",
        "Target Va", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(0.0f, 30.0f, 0.1f, 10, 1)),
    therapy_setting(
        "IEU", As11ProfileId::iVAPS, "AutoEPAPEnable",
        "AutoEPAP", "Therapy", "ivaps_therapy", MODES_IVAPS,
        enum_value(OFF_ON_OPTIONS)),
    therapy_setting(
        "IMN", As11ProfileId::iVAPS, "MinExpiratoryPressure",
        "Min EPAP", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "IMX", As11ProfileId::iVAPS, "MaxExpiratoryPressure",
        "Max EPAP", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "EPI", As11ProfileId::iVAPS, "TargetExpiratoryPressure",
        "EPAP", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "WPM", As11ProfileId::iVAPS, "MinPressureSupport",
        "Min PS", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(4.0f, 30.0f, 0.2f, 50, 1)),
    therapy_setting(
        "WPA", As11ProfileId::iVAPS, "MaxPressureSupport",
        "Max PS", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(4.0f, 30.0f, 0.2f, 50, 1)),
    therapy_setting(
        "IVN", As11ProfileId::iVAPS, "SetMinInspiratoryTime",
        "Ti Min", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(0.1f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "IVX", As11ProfileId::iVAPS, "SetMaxInspiratoryTime",
        "Ti Max", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(0.3f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "IRC", As11ProfileId::iVAPS, "RiseTimeEnable",
        "Rise Time Mode", "Therapy", "ivaps_therapy", MODES_IVAPS,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "IRT", As11ProfileId::iVAPS, "RiseTime",
        "Rise Time", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(150.0f, 900.0f, 50.0f, 1, 0)),
    therapy_setting(
        "IRZ", As11ProfileId::iVAPS, "FallTimeEnable",
        "Fall Time Mode", "Therapy", "ivaps_therapy", MODES_IVAPS,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "IRL", As11ProfileId::iVAPS, "FallTime",
        "Fall Time", "Therapy", "ivaps_therapy", MODES_IVAPS,
        number_value(0.1f, 0.7f, 0.1f, 1000, 1)),
    therapy_setting(
        "VTS", As11ProfileId::iVAPS, "TriggerSensitivity",
        "Trigger", "Therapy", "ivaps_therapy", MODES_IVAPS,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "VCS", As11ProfileId::iVAPS, "CycleSensitivity",
        "Cycle", "Therapy", "ivaps_therapy", MODES_IVAPS,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "IVS", As11ProfileId::iVAPS, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", MODES_IVAPS,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    feature_setting(
        "IHU", "HeightFeature", "HeightDisplayUnit",
        "Height Units", "Configuration", "units", MODES_IVAPS,
        enum_value(HEIGHT_UNIT_OPTIONS)),
    therapy_setting(
        "PA1", As11ProfileId::PAC, "TargetInspiratoryPressure",
        "IPAP", "Therapy", "pac_therapy", MODES_PAC,
        number_value(4.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "PA2", As11ProfileId::PAC, "TargetExpiratoryPressure",
        "EPAP", "Therapy", "pac_therapy", MODES_PAC,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    therapy_setting(
        "PA5", As11ProfileId::PAC, "SetInspiratoryTime",
        "Ti", "Therapy", "pac_therapy", MODES_PAC,
        number_value(0.3f, 4.0f, 0.1f, 50, 1)),
    therapy_setting(
        "PA6", As11ProfileId::PAC, "SetRespiratoryRate",
        "Respiratory Rate", "Therapy", "pac_therapy", MODES_PAC,
        number_value(0.0f, 50.0f, 1.0f, 5, 0)),
    therapy_setting(
        "PA3", As11ProfileId::PAC, "RiseTimeEnable",
        "Rise Time Mode", "Therapy", "pac_therapy", MODES_PAC,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "PA4", As11ProfileId::PAC, "RiseTime",
        "Rise Time", "Therapy", "pac_therapy", MODES_PAC,
        number_value(0.15f, 0.9f, 0.05f, 1000, 2)),
    therapy_setting(
        "P11", As11ProfileId::PAC, "FallTimeEnable",
        "Fall Time Mode", "Therapy", "pac_therapy", MODES_PAC,
        enum_value(MIN_ON_OPTIONS)),
    therapy_setting(
        "P12", As11ProfileId::PAC, "FallTime",
        "Fall Time", "Therapy", "pac_therapy", MODES_PAC,
        number_value(0.1f, 0.7f, 0.1f, 1000, 1)),
    therapy_setting(
        "PA7", As11ProfileId::PAC, "TriggerSensitivity",
        "Trigger", "Therapy", "pac_therapy", MODES_PAC,
        enum_value(SENSITIVITY_OPTIONS, SENSITIVITY_WIRE_OPTIONS)),
    therapy_setting(
        "PA0", As11ProfileId::PAC, "StartPressure",
        "Start EPAP", "Comfort", "therapy_start_pressure", MODES_PAC,
        number_value(2.0f, 25.0f, 0.2f, 50, 1)),
    feature_setting(
        "AFC", "ComfortFeature", "AutoSetComfort",
        "AutoSet Response", "Comfort", "comfort", 0x0002u,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "EPX", "EprFeature", "EprEnable",
        "EPR Enable", "Comfort", "comfort", 0x0007u,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "EPT", "EprFeature", "EprType",
        "EPR Type", "Comfort", "comfort", 0x0007u,
        enum_value(EPR_TYPE_OPTIONS)),
    feature_setting(
        "EPR", "EprFeature", "EprPressure",
        "EPR Level", "Comfort", "comfort", 0x0007u,
        number_value(1.0f, 3.0f, 1.0f, 50, 0)),
    feature_setting(
        "RMT", "AutoRampFeature", "RampTime",
        "Ramp Time", "Comfort", "ramp", 0x07FFu,
        number_value(5.0f, 45.0f, 5.0f, 1, 0)),
    feature_setting(
        "RMA", "AutoRampFeature", "RampEnable",
        "Ramp Mode", "Comfort", "ramp", 0x07FFu,
        enum_value(OFF_ON_AUTO_OPTIONS)),
    feature_setting(
        "SST", "SmartStartStopFeature", "SmartStart",
        "SmartStart", "Preferences", "therapy_behavior", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "SSP", "SmartStartStopFeature", "SmartStop",
        "SmartStop", "Preferences", "therapy_behavior", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "MSK", "CircuitFeature", "MaskType",
        "Mask Setting", "Circuit", "circuit", 0x07FFu,
        enum_value(MASK_TYPE_OPTIONS)),
    feature_setting(
        "TBT", "CircuitFeature", "TubeType",
        "Tube", "Circuit", "circuit", 0x07FFu,
        enum_value(TUBE_TYPE_OPTIONS, TUBE_TYPE_WIRE_OPTIONS)),
    feature_setting(
        "ABF", "CircuitFeature", "AntiBacterialFilter",
        "B/V Filter", "Circuit", "circuit", 0x07FFu,
        enum_value(YES_NO_WIRE_ORDER_OPTIONS)),
    feature_setting(
        "CCO", "ClimateFeature", "ClimateControl",
        "Climate Control", "Comfort", "climate", 0x07FFu,
        enum_value(AUTO_MANUAL_OPTIONS)),
    feature_setting(
        "HMX", "ClimateFeature", "HumidifierSettingEnable",
        "Humidity Mode", "Comfort", "climate", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "HMS", "ClimateFeature", "HumidifierLevel",
        "Humidity Level", "Comfort", "climate", 0x07FFu,
        number_value(1.0f, 8.0f, 1.0f, 1, 0)),
    feature_setting(
        "HTX", "ClimateFeature", "HeatedTubeSettingEnable",
        "Tube Heating", "Comfort", "climate", 0x07FFu,
        enum_value(OFF_ON_AUTO_OPTIONS)),
    feature_setting(
        "HTS", "ClimateFeature", "HeatedTubeTemperature",
        "Tube Temp.", "Comfort", "climate", 0x07FFu,
        number_value(15.6f, 30.0f, 0.1f, 10, 1)),
    feature_setting(
        "EXH", "ClimateFeature", "ExternalHumidifier",
        "External Humidifier", "Comfort", "climate", MODES_NONE,
        enum_value(NO_YES_WIRE_ORDER_OPTIONS)),
    feature_setting(
        "LNC", "LanguageFeature", "LanguageConfiguration",
        "Language Configuration", "Preferences", "locale", MODES_NONE,
        number_value(0.0f, 134217727.0f, 1.0f, 1, 0)),
    feature_setting(
        "LAN", "LanguageFeature", "Language",
        "Language", "Preferences", "locale", 0x07FFu,
        enum_value(LANGUAGE_OPTIONS, LANGUAGE_WIRE_OPTIONS)),
    feature_setting(
        "SLS", "LanguageFeature", "LanguageSelection",
        "Language Selection", "Preferences", "locale", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "SCP", "UserSolutionFeature", "SurveyPersonalise",
        "Survey Personalise", "Device", "survey", MODES_NONE,
        enum_value(NO_YES_NOT_ANSWERED_OPTIONS)),
    feature_setting(
        "TMU", "TemperatureFeature", "TemperatureUnit",
        "Temp. Units", "Preferences", "locale", 0x07FFu,
        enum_value(TEMPERATURE_UNIT_OPTIONS)),
    feature_setting(
        "ACC", "PatientViewFeature", "PatientView",
        "Patient View", "Preferences", "patient_access", 0x07FFu,
        enum_value(PATIENT_VIEW_OPTIONS)),
    feature_setting(
        "EPA", "EprFeature", "EprEnablePatientAccess",
        "EPR Patient Access", "Preferences", "patient_access", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "RPE", "AutoRampFeature", "RampEnablePatientAccess",
        "Ramp Patient Access", "Preferences", "patient_access", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "DAH", "PatientViewFeature", "DisplayAHI",
        "Display AHI", "Preferences", "patient_access", 0x07FFu,
        enum_value(NO_YES_WIRE_ORDER_OPTIONS)),
    feature_setting(
        "TZO", "TimeZoneFeature", "TimeZoneOffset",
        "Time Zone Offset", "Preferences", "locale_time", MODES_NONE,
        number_value(-720.0f, 840.0f, 60.0f, 1, 0)),
    feature_setting(
        "MAI", "CareCheckFeature", "CareCheckToggle",
        "Care Check", "Device", "device_check", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "SCO", "DeviceHealthFeature", "SoundcheckFeatureToggle",
        "Soundcheck", "Device", "device_health", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "SCK", "DeviceHealthFeature", "SoundcheckRunFrequency",
        "Device Diagnostic", "Device", "device_health", MODES_NONE,
        enum_value(REMINDER_FREQUENCY_OPTIONS)),
    feature_setting(
        "RIM", "ReminderFeature.ReminderMask", "Enable",
        "Enable", "Reminders", "reminder", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "RTM", "ReminderFeature.ReminderMask", "StartDateTime",
        "Start Date", "Reminders", "reminder", MODES_NONE,
        number_value(0.0f, 2147483647.0f, 1.0f, 1, 0)),
    feature_setting(
        "RDM", "ReminderFeature.ReminderMask", "Period",
        "Period", "Reminders", "reminder", MODES_NONE,
        enum_value(MONTH_PERIOD_OPTIONS, MONTH_PERIOD_WIRE_OPTIONS)),
    feature_setting(
        "RIT", "ReminderFeature.ReminderTubing", "Enable",
        "Enable", "Reminders", "reminder", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "RTT", "ReminderFeature.ReminderTubing", "StartDateTime",
        "Start Date", "Reminders", "reminder", MODES_NONE,
        number_value(0.0f, 2147483647.0f, 1.0f, 1, 0)),
    feature_setting(
        "RDT", "ReminderFeature.ReminderTubing", "Period",
        "Period", "Reminders", "reminder", MODES_NONE,
        enum_value(MONTH_PERIOD_OPTIONS, MONTH_PERIOD_WIRE_OPTIONS)),
    feature_setting(
        "RIF", "ReminderFeature.ReminderFilter", "Enable",
        "Enable", "Reminders", "reminder", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "RTF", "ReminderFeature.ReminderFilter", "StartDateTime",
        "Start Date", "Reminders", "reminder", MODES_NONE,
        number_value(0.0f, 2147483647.0f, 1.0f, 1, 0)),
    feature_setting(
        "RDF", "ReminderFeature.ReminderFilter", "Period",
        "Period", "Reminders", "reminder", MODES_NONE,
        enum_value(MONTH_PERIOD_OPTIONS, MONTH_PERIOD_WIRE_OPTIONS)),
    feature_setting(
        "RIC", "ReminderFeature.ReminderHumidifier", "Enable",
        "Enable", "Reminders", "reminder", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "RTH", "ReminderFeature.ReminderHumidifier", "StartDateTime",
        "Start Date", "Reminders", "reminder", MODES_NONE,
        number_value(0.0f, 2147483647.0f, 1.0f, 1, 0)),
    feature_setting(
        "RDH", "ReminderFeature.ReminderHumidifier", "Period",
        "Period", "Reminders", "reminder", MODES_NONE,
        enum_value(MONTH_PERIOD_OPTIONS, MONTH_PERIOD_WIRE_OPTIONS)),
    feature_setting(
        "TUD", "DisplayFeature", "TotalUsedHoursDisplayToggle",
        "Total Used Hours", "Preferences", "display", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "SSE", "DisplayFeature", "SplashScreenDisplaySelection",
        "Splash Screen", "Preferences", "display", MODES_NONE,
        enum_value(SPLASH_SCREEN_OPTIONS)),
    feature_setting(
        "SSY", "DisplayFeature", "CycleDisplayFormat",
        "Cycle Display Format", "Preferences", "display", MODES_NONE,
        enum_value(STRING_PERCENTAGE_OPTIONS)),
    feature_setting(
        "CCA", "DisplayFeature", "CareCheckInAvailable",
        "Care Check-In", "Device", "device_check", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "MAS", "DisplayFeature", "MyAirScreens",
        "myAir", "Preferences", "display", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "CFC", "DisplayFeature", "ClinicalConfirmation",
        "Confirm Clinical Menu", "Preferences", "patient_access", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "BBE", "DisplayFeature", "DynamicMessageToggle",
        "Notifications", "Preferences", "display", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "SCF", "ConfirmStopFeature", "ConfirmStopEnable",
        "Confirm Stop", "Preferences", "therapy_behavior", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "TLF", "TherapyLEDFeature", "TherapyLEDAlwaysOn",
        "Therapy LED", "Preferences", "therapy_behavior", 0x07FFu,
        enum_value(OFF_ON_OPTIONS)),
    feature_setting(
        "MKD", "MaskSenseFeature", "MaskSenseToggle",
        "AcousticSignal", "Device", "mask_sense", MODES_NONE,
        enum_value(OFF_ON_OPTIONS)),
};

const As11SettingCompositeDef SETTING_COMPOSITES[] = {
    {
        "RMA+RMT",
        "Ramp Time",
        "RMA",
        "RMT",
        1,
        "Comfort",
        "ramp",
        RAMP_TIME_COMPOSITE_OPTIONS,
        option_count(RAMP_TIME_COMPOSITE_OPTIONS)
    },
    {
        "HMX+HMS",
        "Humidity Level",
        "HMX",
        "HMS",
        1,
        "Comfort",
        "climate",
        HUMIDITY_LEVEL_COMPOSITE_OPTIONS,
        option_count(HUMIDITY_LEVEL_COMPOSITE_OPTIONS)
    },
    {
        "ZZ9+Z10",
        "Rise Time",
        "ZZ9",
        "Z10",
        1,
        "Therapy",
        "timing",
        RISE_TIME_COMPOSITE_OPTIONS,
        option_count(RISE_TIME_COMPOSITE_OPTIONS)
    },
    {
        "Z16+Z17",
        "Fall Time",
        "Z16",
        "Z17",
        1,
        "Therapy",
        "timing",
        FALL_TIME_COMPOSITE_OPTIONS,
        option_count(FALL_TIME_COMPOSITE_OPTIONS)
    },
    {
        "XA9+XAA",
        "Rise Time",
        "XA9",
        "XAA",
        1,
        "Therapy",
        "timing",
        RISE_TIME_COMPOSITE_OPTIONS,
        option_count(RISE_TIME_COMPOSITE_OPTIONS)
    },
    {
        "XAM+XAP",
        "Fall Time",
        "XAM",
        "XAP",
        1,
        "Therapy",
        "timing",
        FALL_TIME_COMPOSITE_OPTIONS,
        option_count(FALL_TIME_COMPOSITE_OPTIONS)
    },
    {
        "XB6+XB7",
        "Rise Time",
        "XB6",
        "XB7",
        1,
        "Therapy",
        "timing",
        RISE_TIME_COMPOSITE_OPTIONS,
        option_count(RISE_TIME_COMPOSITE_OPTIONS)
    },
    {
        "IRC+IRT",
        "Rise Time",
        "IRC",
        "IRT",
        1,
        "Therapy",
        "ivaps_therapy",
        RISE_TIME_COMPOSITE_OPTIONS,
        option_count(RISE_TIME_COMPOSITE_OPTIONS)
    },
    {
        "IRZ+IRL",
        "Fall Time",
        "IRZ",
        "IRL",
        1,
        "Therapy",
        "ivaps_therapy",
        FALL_TIME_COMPOSITE_OPTIONS,
        option_count(FALL_TIME_COMPOSITE_OPTIONS)
    },
    {
        "PA3+PA4",
        "Rise Time",
        "PA3",
        "PA4",
        1,
        "Therapy",
        "pac_therapy",
        RISE_TIME_COMPOSITE_OPTIONS,
        option_count(RISE_TIME_COMPOSITE_OPTIONS)
    },
    {
        "P11+P12",
        "Fall Time",
        "P11",
        "P12",
        1,
        "Therapy",
        "pac_therapy",
        FALL_TIME_COMPOSITE_OPTIONS,
        option_count(FALL_TIME_COMPOSITE_OPTIONS)
    },
};
