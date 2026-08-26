#include "edf_str_settings.h"

#include <ArduinoJson.h>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "edf_str_signal_table.h"
#include "report_parser.h"

namespace aircannect {
namespace {

static constexpr size_t EDF_STR_GET_NAMES_RESERVE = 512;
static constexpr uint64_t EDF_STR_SUMMARY_SESSION_TOLERANCE_MS =
    2ULL * 60ULL * 1000ULL;

struct EdfStrSummaryFieldMap {
    const char *tag = nullptr;
    ReportSummaryField field = ReportSummaryField::Count;
};

static constexpr EdfStrSummaryFieldMap STR_SUMMARY_FIELDS[] = {
#define EDF_STR_SUMMARY_FIELD(tag, field) \
    {tag, ReportSummaryField::field},
#include "edf_str_summary_fields.inc"
#undef EDF_STR_SUMMARY_FIELD
};
static_assert(sizeof(STR_SUMMARY_FIELDS) / sizeof(STR_SUMMARY_FIELDS[0]) ==
                  AC_REPORT_SUMMARY_FIELD_COUNT,
              "STR mapping must cover every Summary field");

bool parse_float_text(const char *text, float &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (!end || *end != 0 || !isfinite(value)) return false;
    out = value;
    return true;
}

bool parse_iso8601_seconds(const char *text, float &out) {
    if (!text || text[0] != 'P' || text[1] != 'T') return false;
    const char *value = text + 2;
    char *end = nullptr;
    const float seconds = strtof(value, &end);
    if (!end || *end != 'S' || end[1] != 0 || !isfinite(seconds)) {
        return false;
    }
    out = seconds;
    return true;
}

bool rpc_name_uses_msec_physical(const char *rpc_name) {
    return rpc_name &&
           (strcmp(rpc_name, "_Z10") == 0 ||
            strcmp(rpc_name, "_XAA") == 0 ||
            strcmp(rpc_name, "_XB7") == 0);
}

void rpc_name_for_str_tag(const char *tag, char *out, size_t out_size) {
    if (!out || !out_size) return;
    out[0] = 0;
    if (!tag || !tag[0]) return;
    if (tag[0] == '_') {
        snprintf(out, out_size, "%s", tag);
        return;
    }
    snprintf(out, out_size, "_%s", tag);
}

bool physical_from_json_value(JsonVariantConst value,
                              const char *rpc_name,
                              float &physical) {
    if (value.is<bool>()) {
        physical = value.as<bool>() ? 1.0f : 0.0f;
        return true;
    }
    if (value.is<float>() || value.is<int>() || value.is<long>()) {
        physical = value.as<float>();
        return true;
    }
    if (!value.is<const char *>()) return false;

    const char *text = value.as<const char *>();
    int16_t option_index = 0;
    if (parse_iso8601_seconds(text, physical)) {
        if (rpc_name_uses_msec_physical(rpc_name)) physical *= 1000.0f;
        return true;
    }
    if (parse_float_text(text, physical)) {
        return true;
    }
    if (as11_setting_option_index_for_rpc_name(rpc_name, text, option_index)) {
        physical = static_cast<float>(option_index);
        return true;
    }
    return false;
}

enum class EdfStrNumericInput : uint8_t {
    OptionIndex,
    DigitalCode,
};

struct EdfStrDigitalRemap {
    const char *rpc_name = nullptr;
    const int16_t *codes = nullptr;
    size_t code_count = 0;
    EdfStrNumericInput numeric_input = EdfStrNumericInput::OptionIndex;
};

static constexpr int16_t STR_MODE_CODES[] = {
        3,  // CPAP
        1,  // AutoSet
        2,  // AutoSet For Her
        4,  // S
        10, // ST
        16, // T
        8,  // VAuto
        6,  // ASV
        7,  // ASVAuto
        5,  // iVAPS
        9,  // PAC
};
static constexpr int16_t STR_SENSITIVITY_CODES[] = {1, 2, 3, 4, 5, 6, 7};
static constexpr int16_t STR_BOOL_CODES[] = {1, 2};
static constexpr int16_t STR_THREE_OPTION_CODES[] = {1, 2, 3};
static constexpr int16_t STR_SPONT_RESP_RATE_ENABLE_CODES[] = {1, 3};
static constexpr int16_t STR_TUBE_TYPE_CODES[] = {3, 4, 1};

// AS11/ResScan STR enum export maps. Labels resolve through the setting option
// table; numeric DigitalCode values are already STR-native. Some tags are
// proven by firmware maps, others by native EDF comparison for the current AS11
// setting strings.
static constexpr EdfStrDigitalRemap STR_DIGITAL_REMAPS[] = {
    {"_MOP", STR_MODE_CODES,
     sizeof(STR_MODE_CODES) / sizeof(STR_MODE_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_XE6", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XE7", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_Z11", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_Z12", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_ZU1", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XAB", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_AFC", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_ZZ4", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_ZZ9", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_Z16", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XAM", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XB6", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XA9", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XB9", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_ZZ5", STR_SPONT_RESP_RATE_ENABLE_CODES,
     sizeof(STR_SPONT_RESP_RATE_ENABLE_CODES) /
         sizeof(STR_SPONT_RESP_RATE_ENABLE_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_RMA", STR_THREE_OPTION_CODES,
     sizeof(STR_THREE_OPTION_CODES) / sizeof(STR_THREE_OPTION_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_EPA", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_EPX", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_EPT", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_SST", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_ABF", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_TBT", STR_TUBE_TYPE_CODES,
     sizeof(STR_TUBE_TYPE_CODES) / sizeof(STR_TUBE_TYPE_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_CCO", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_HMX", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_HTX", STR_THREE_OPTION_CODES,
     sizeof(STR_THREE_OPTION_CODES) / sizeof(STR_THREE_OPTION_CODES[0]),
     EdfStrNumericInput::OptionIndex},
};

bool parse_integer_text(const char *text, int16_t &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (!end || *end != 0 || value < INT16_MIN || value > INT16_MAX) {
        return false;
    }
    out = static_cast<int16_t>(value);
    return true;
}

bool integer_from_json_value(JsonVariantConst value, int16_t &out) {
    if (value.is<int>() || value.is<long>()) {
        const long parsed = value.as<long>();
        if (parsed < INT16_MIN || parsed > INT16_MAX) return false;
        out = static_cast<int16_t>(parsed);
        return true;
    }
    if (value.is<float>()) {
        const float parsed = value.as<float>();
        if (!isfinite(parsed) || parsed < static_cast<float>(INT16_MIN) ||
            parsed > static_cast<float>(INT16_MAX)) {
            return false;
        }
        const long rounded = lroundf(parsed);
        if (fabsf(parsed - static_cast<float>(rounded)) > 0.0001f) {
            return false;
        }
        out = static_cast<int16_t>(rounded);
        return true;
    }
    return false;
}

bool text_equals(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

bool str_text_digital_from_json_value(JsonVariantConst value,
                                      const char *rpc_name,
                                      int16_t &digital) {
    if (!text_equals(rpc_name, "_MSK") || !value.is<const char *>()) {
        return false;
    }

    const char *text = value.as<const char *>();
    if (text_equals(text, "Pillows")) {
        digital = 2;
        return true;
    }
    return false;
}

bool str_text_digital_map_is_exclusive(const char *rpc_name) {
    return text_equals(rpc_name, "_MSK");
}

bool fallback_option_index_from_text(const char *text, int16_t &index) {
    if (text_equals(text, "Off") || text_equals(text, "No") ||
        text_equals(text, "Min")) {
        index = 0;
        return true;
    }
    if (text_equals(text, "On") || text_equals(text, "Yes")) {
        index = 1;
        return true;
    }
    return false;
}

bool str_option_index_override_from_text(const char *rpc_name,
                                         const char *text,
                                         int16_t &index) {
    if (!text_equals(rpc_name, "_ABF")) return false;

    // The live setting catalog follows the firmware option order for ABF
    // (Yes, No), while native STR exports use the common boolean digital order
    // (No, Yes) before remapping to EDF codes.
    return fallback_option_index_from_text(text, index);
}

bool option_index_from_text(const char *text,
                            const char *rpc_name,
                            int16_t &index) {
    if (str_option_index_override_from_text(rpc_name, text, index)) {
        return true;
    }
    if (as11_setting_option_index_for_rpc_name(rpc_name, text, index)) {
        return true;
    }
    if (strcmp(rpc_name, "_MOP") == 0) {
        const int parsed = as11_mode_index_from_value(text);
        if (parsed >= 0 && parsed <= 10) {
            index = static_cast<int16_t>(parsed);
            return true;
        }
    }
    return fallback_option_index_from_text(text, index);
}

const EdfStrDigitalRemap *str_digital_remap_for_rpc_name(
    const char *rpc_name) {
    if (!rpc_name) return nullptr;
    for (const EdfStrDigitalRemap &remap : STR_DIGITAL_REMAPS) {
        if (strcmp(remap.rpc_name, rpc_name) == 0) return &remap;
    }
    return nullptr;
}

bool digital_from_option_index(const EdfStrDigitalRemap &remap,
                               int16_t option_index,
                               int16_t &digital_value) {
    if (option_index < 0 ||
        option_index >= static_cast<int16_t>(remap.code_count)) {
        return false;
    }

    digital_value = remap.codes[option_index];
    return true;
}

bool digital_code_is_allowed(const EdfStrDigitalRemap &remap,
                             int16_t code) {
    for (size_t i = 0; i < remap.code_count; ++i) {
        if (remap.codes[i] == code) return true;
    }
    return false;
}

bool str_digital_from_json_value(JsonVariantConst value,
                                 const char *rpc_name,
                                 const EdfStrDigitalRemap &remap,
                                 int16_t &digital_value) {
    if (value.is<bool>()) {
        return digital_from_option_index(remap,
                                         value.as<bool>() ? 1 : 0,
                                         digital_value);
    }

    int16_t numeric_value = 0;
    if (integer_from_json_value(value, numeric_value)) {
        if (remap.numeric_input == EdfStrNumericInput::DigitalCode) {
            if (!digital_code_is_allowed(remap, numeric_value)) return false;
            digital_value = numeric_value;
            return true;
        }
        return digital_from_option_index(remap, numeric_value, digital_value);
    }

    if (!value.is<const char *>()) return false;

    const char *text = value.as<const char *>();
    if (parse_integer_text(text, numeric_value)) {
        if (remap.numeric_input == EdfStrNumericInput::DigitalCode) {
            if (!digital_code_is_allowed(remap, numeric_value)) return false;
            digital_value = numeric_value;
            return true;
        }
        return digital_from_option_index(remap, numeric_value, digital_value);
    }

    int16_t option_index = 0;
    if (!option_index_from_text(text, rpc_name, option_index)) return false;
    return digital_from_option_index(remap, option_index, digital_value);
}

JsonObjectConst get_value_object(const JsonDocument &doc) {
    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (!result.isNull()) return result;
    return doc["error"]["data"].as<JsonObjectConst>();
}

bool append_str_get_name(std::string &names, const char *tag) {
    char rpc_name[8] = {};
    rpc_name_for_str_tag(tag, rpc_name, sizeof(rpc_name));
    if (!rpc_name[0]) return false;
    if (!names.empty()) names += ' ';
    names += rpc_name;
    return true;
}

bool summary_code_from_index(uint32_t index,
                             const int16_t *codes,
                             size_t code_count,
                             int16_t &digital) {
    if (index >= code_count) return false;
    digital = codes[index];
    return true;
}

bool summary_tube_connected_code(uint32_t index, int16_t &digital) {
    static constexpr int16_t kCodes[] = {3, 4, 1, 5, 2};
    return summary_code_from_index(index,
                                   kCodes,
                                   sizeof(kCodes) / sizeof(kCodes[0]),
                                   digital);
}

bool summary_humidifier_connected_code(uint32_t index,
                                       int16_t &digital) {
    static constexpr int16_t kCodes[] = {1, 2, 3};
    return summary_code_from_index(index,
                                   kCodes,
                                   sizeof(kCodes) / sizeof(kCodes[0]),
                                   digital);
}

bool summary_digital_from_raw(ReportSummaryField field,
                              uint32_t raw,
                              int16_t &digital) {
    if (field == ReportSummaryField::TubeConnected) {
        return summary_tube_connected_code(raw, digital);
    }
    if (field == ReportSummaryField::HumidifierConnected) {
        return summary_humidifier_connected_code(raw, digital);
    }
    return false;
}

struct SummarySpoolApplyContext {
    uint16_t sleep_day = 0;
    uint64_t session_start_ms = 0;
    uint64_t session_end_ms = 0;
    EdfStrSessionAccumulator *session = nullptr;
    EdfStrSummaryApplyResult *result = nullptr;
};

bool summary_timestamp_within(uint64_t lhs, uint64_t rhs) {
    return lhs >= rhs
        ? lhs - rhs <= EDF_STR_SUMMARY_SESSION_TOLERANCE_MS
        : rhs - lhs <= EDF_STR_SUMMARY_SESSION_TOLERANCE_MS;
}

bool summary_record_contains_session(const ReportSummaryRecord &record,
                                     uint64_t session_start_ms,
                                     uint64_t session_end_ms) {
    for (uint32_t i = 0; i < record.session_interval_count; ++i) {
        const ReportSummarySession &candidate = record.sessions[i];
        const uint64_t duration_ms =
            static_cast<uint64_t>(candidate.duration_min) * 60ULL * 1000ULL;
        if (!candidate.start_ms || !duration_ms ||
            candidate.start_ms > UINT64_MAX - duration_ms) {
            continue;
        }

        const uint64_t candidate_end_ms = candidate.start_ms + duration_ms;
        if (summary_timestamp_within(candidate.start_ms, session_start_ms) &&
            summary_timestamp_within(candidate_end_ms, session_end_ms)) {
            return true;
        }
    }
    return false;
}

bool apply_summary_spool_record(void *opaque,
                                const ReportSummaryRecord &record) {
    SummarySpoolApplyContext *context =
        static_cast<SummarySpoolApplyContext *>(opaque);
    if (!context || !context->session || !context->result ||
        context->result->record_found) {
        return true;
    }

    int32_t sleep_day = 0;
    if (!report_summary_sleep_day_epoch_days(record, sleep_day) ||
        sleep_day != context->sleep_day ||
        !summary_record_contains_session(record,
                                         context->session_start_ms,
                                         context->session_end_ms)) {
        return true;
    }

    EdfStrSettingsApplyResult applied;
    if (!edf_str_apply_summary_record(record, *context->session, applied)) {
        context->result->error = applied.error;
        return true;
    }

    context->result->record_found = true;
    context->result->values = applied.values;
    context->result->missing = applied.missing;
    context->result->unmapped = applied.unmapped;
    return true;
}

}  // namespace

std::string edf_str_setting_get_names() {
    std::string names;
    names.reserve(EDF_STR_GET_NAMES_RESERVE);
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::SettingGet ||
            !signal->short_tag) {
            continue;
        }

        (void)append_str_get_name(names, signal->short_tag);
    }
    return names;
}

bool edf_str_summary_field_for_tag(const char *tag,
                                   ReportSummaryField &out) {
    if (!tag || !tag[0]) return false;

    for (const EdfStrSummaryFieldMap &mapping : STR_SUMMARY_FIELDS) {
        if (strcmp(tag, mapping.tag) != 0) continue;

        out = mapping.field;
        return true;
    }
    return false;
}

bool edf_str_apply_settings_response(RpcPayloadView payload,
                                     EdfStrSessionAccumulator &session,
                                     EdfStrSettingsApplyResult &result) {
    result = {};

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, payload.data() ? payload.data() : "", payload.size());
    if (err) {
        result.error = "str_settings_json_failed";
        return false;
    }

    JsonObjectConst json_result = get_value_object(doc);
    if (json_result.isNull()) {
        result.error = json_member_present(payload.data(), payload.size(),
                                           "error")
                           ? "str_settings_rpc_error"
                           : "str_settings_missing_result";
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::SettingGet ||
            !signal->short_tag) {
            continue;
        }

        char rpc_name[8] = {};
        rpc_name_for_str_tag(signal->short_tag, rpc_name, sizeof(rpc_name));
        JsonVariantConst value = json_result[rpc_name];
        if (value.isNull()) {
            result.missing++;
            continue;
        }

        int16_t digital = 0;
        if (str_text_digital_from_json_value(value, rpc_name, digital)) {
            if (session.set_signal_digital(i, digital)) {
                result.values++;
            } else {
                result.unmapped++;
            }
            continue;
        }
        if (str_text_digital_map_is_exclusive(rpc_name)) {
            result.unmapped++;
            continue;
        }

        const EdfStrDigitalRemap *remap =
            str_digital_remap_for_rpc_name(rpc_name);
        if (remap) {
            if (str_digital_from_json_value(value, rpc_name, *remap, digital) &&
                session.set_signal_digital(i, digital)) {
                result.values++;
            } else {
                result.unmapped++;
            }
            continue;
        }

        float physical = 0.0f;
        if (physical_from_json_value(value, rpc_name, physical) &&
            session.set_signal_physical(i, physical)) {
            result.values++;
        } else {
            result.unmapped++;
        }
    }

    result.ok = true;
    return true;
}

bool edf_str_apply_summary_record(const ReportSummaryRecord &record,
                                  EdfStrSessionAccumulator &session,
                                  EdfStrSettingsApplyResult &result) {
    result = {};
    if (!record.valid) {
        result.error = "str_summary_record_invalid";
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::Summary ||
            !signal->short_tag) {
            continue;
        }

        ReportSummaryField field = ReportSummaryField::Count;
        if (!edf_str_summary_field_for_tag(signal->short_tag, field)) {
            result.unmapped++;
            continue;
        }

        uint32_t raw = 0;
        if (!report_summary_field_value(record, field, raw)) {
            result.missing++;
            continue;
        }

        int16_t digital = 0;
        if (report_summary_field_encoding(field) ==
            ReportSummaryValueEncoding::EnumIndex) {
            if (!summary_digital_from_raw(field, raw, digital)) {
                result.unmapped++;
                continue;
            }
            if (session.set_signal_digital(i, digital)) {
                result.values++;
            } else {
                result.unmapped++;
            }
            continue;
        }

        float physical = 0.0f;
        if (report_summary_field_physical_value(record, field, physical) &&
            session.set_signal_physical(i, physical)) {
            result.values++;
        } else {
            result.unmapped++;
        }
    }

    result.ok = true;
    return true;
}

bool edf_str_apply_summary_spool(const ReportSpoolResult &spool,
                                 uint16_t sleep_day,
                                 uint64_t session_start_ms,
                                 uint64_t session_end_ms,
                                 EdfStrSessionAccumulator &session,
                                 EdfStrSummaryApplyResult &result) {
    result = {};

    SummarySpoolApplyContext context;
    context.sleep_day = sleep_day;
    context.session_start_ms = session_start_ms;
    context.session_end_ms = session_end_ms;
    context.session = &session;
    context.result = &result;

    char error[64] = {};
    if (!report_parse_summary_spool(spool,
                                    apply_summary_spool_record,
                                    &context,
                                    error,
                                    sizeof(error))) {
        result.error = "str_summary_spool_invalid";
        return false;
    }
    if (result.error) return false;

    result.ok = true;
    return true;
}

}  // namespace aircannect
