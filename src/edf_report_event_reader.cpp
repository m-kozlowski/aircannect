#include "edf_report_event_reader.h"

#include <string.h>

#include "edf_annotation_labels.h"
#include "edf_bytes.h"
#include "edf_file_writer.h"

namespace aircannect {
namespace {

static constexpr uint8_t TAL_ANNOTATION_SEP = 0x14;
static constexpr uint8_t TAL_DURATION_SEP = 0x15;

bool is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

bool parse_milliseconds_fraction(const uint8_t *data,
                                 size_t len,
                                 size_t &index,
                                 int64_t &value_ms) {
    if (index >= len || data[index] != '.') return true;
    ++index;
    if (index >= len || !is_digit(data[index])) return false;

    uint32_t fraction_ms = 0;
    uint32_t scale = 100;
    while (index < len && is_digit(data[index])) {
        if (scale > 0) {
            fraction_ms += static_cast<uint32_t>(data[index] - '0') * scale;
            scale /= 10;
        }
        ++index;
    }
    value_ms += fraction_ms;
    return true;
}

bool parse_signed_milliseconds(const uint8_t *data,
                               size_t len,
                               size_t &index,
                               int64_t &out) {
    out = 0;
    if (!data || index >= len) return false;
    int sign = 1;
    if (data[index] == '+') {
        ++index;
    } else if (data[index] == '-') {
        sign = -1;
        ++index;
    } else {
        return false;
    }
    if (index >= len || !is_digit(data[index])) return false;

    int64_t value = 0;
    while (index < len && is_digit(data[index])) {
        value = value * 10 + static_cast<int64_t>(data[index] - '0');
        if (value > INT64_MAX / 1000LL) return false;
        ++index;
    }
    value *= 1000LL;
    if (!parse_milliseconds_fraction(data, len, index, value)) return false;
    out = value * sign;
    return true;
}

bool parse_unsigned_milliseconds(const uint8_t *data,
                                 size_t len,
                                 size_t &index,
                                 int64_t &out) {
    out = 0;
    if (!data || index >= len || !is_digit(data[index])) return false;

    int64_t value = 0;
    while (index < len && is_digit(data[index])) {
        value = value * 10 + static_cast<int64_t>(data[index] - '0');
        if (value > INT64_MAX / 1000LL) return false;
        ++index;
    }
    value *= 1000LL;
    if (!parse_milliseconds_fraction(data, len, index, value)) return false;
    out = value;
    return true;
}

bool event_code_for_label(EdfInventoryFileKind kind,
                          const uint8_t *label,
                          size_t len,
                          uint16_t &code) {
    EdfAnnotationLabelId id = EdfAnnotationLabelId::Hypopnea;
    if (!edf_annotation_label_id_for_text(kind, label, len, id)) return false;
    switch (id) {
        case EdfAnnotationLabelId::Hypopnea:
            code = report_event_code_value(ReportEventCode::Hypopnea);
            return true;
        case EdfAnnotationLabelId::CentralApnea:
            code = report_event_code_value(ReportEventCode::CentralApnea);
            return true;
        case EdfAnnotationLabelId::ObstructiveApnea:
            code = report_event_code_value(ReportEventCode::ObstructiveApnea);
            return true;
        case EdfAnnotationLabelId::UnclassifiedApnea:
            code = report_event_code_value(ReportEventCode::UnclassifiedApnea);
            return true;
        case EdfAnnotationLabelId::Arousal:
            code = report_event_code_value(ReportEventCode::Arousal);
            return true;
        case EdfAnnotationLabelId::CsrStart:
        case EdfAnnotationLabelId::CsrEnd:
            return false;
    }
    return false;
}

bool verify_record_crc(const uint8_t *record, size_t record_size) {
    if (!record || record_size < 2) return false;
    const size_t payload_size = record_size - 2;
    const uint16_t expected =
        static_cast<uint16_t>(edf_read_i16_le(record + payload_size));
    const uint16_t actual = edf_crc16_ccitt_false(record, payload_size);
    return expected == actual;
}

}  // namespace

const char *edf_report_event_status_name(EdfReportEventStatus status) {
    switch (status) {
        case EdfReportEventStatus::Ok: return "ok";
        case EdfReportEventStatus::InvalidArgument:
            return "invalid_argument";
        case EdfReportEventStatus::CrcError: return "crc_error";
        case EdfReportEventStatus::ParseError: return "parse_error";
        case EdfReportEventStatus::CallbackRejected:
            return "callback_rejected";
        default:
            return "unknown";
    }
}

EdfReportEventStatus edf_report_decode_annotation_record(
    const EdfReportFileDescriptor &file,
    const uint8_t *record,
    size_t record_size,
    bool verify_crc,
    EdfReportEventCallback callback,
    void *context,
    EdfReportEventDecodeStats &stats) {
    if (file.status != EdfReportFileStatus::Ok || !record || !callback ||
        file.header_start_ms <= 0 ||
        (file.inventory.kind != EdfInventoryFileKind::Eve &&
         file.inventory.kind != EdfInventoryFileKind::Csl) ||
        record_size < 2) {
        return EdfReportEventStatus::InvalidArgument;
    }
    if (verify_crc && !verify_record_crc(record, record_size)) {
        return EdfReportEventStatus::CrcError;
    }

    const size_t payload_size = record_size - 2;
    size_t index = 0;
    while (index < payload_size) {
        while (index < payload_size && record[index] == 0) ++index;
        if (index >= payload_size) break;

        int64_t onset_ms = 0;
        if (!parse_signed_milliseconds(record,
                                       payload_size,
                                       index,
                                       onset_ms)) {
            return EdfReportEventStatus::ParseError;
        }

        int64_t duration_ms = 0;
        if (index < payload_size && record[index] == TAL_DURATION_SEP) {
            ++index;
            if (!parse_unsigned_milliseconds(record,
                                             payload_size,
                                             index,
                                             duration_ms)) {
                return EdfReportEventStatus::ParseError;
            }
        }
        if (index >= payload_size || record[index] != TAL_ANNOTATION_SEP) {
            return EdfReportEventStatus::ParseError;
        }
        ++index;

        while (index < payload_size && record[index] != 0) {
            const size_t label_start = index;
            while (index < payload_size &&
                   record[index] != TAL_ANNOTATION_SEP &&
                   record[index] != 0) {
                ++index;
            }
            const size_t label_len = index - label_start;
            if (label_len > 0) {
                stats.annotations_seen++;
                uint16_t code = 0;
                if (event_code_for_label(file.inventory.kind,
                                         record + label_start,
                                         label_len,
                                         code)) {
                    ReportEventRecord event;
                    event.start_ms = file.header_start_ms + onset_ms;
                    event.duration_ms = duration_ms > INT32_MAX
                                            ? INT32_MAX
                                            : static_cast<int32_t>(duration_ms);
                    event.code = code;
                    event.flags = 0;
                    if (!callback(context, event)) {
                        return EdfReportEventStatus::CallbackRejected;
                    }
                    stats.events_emitted++;
                } else {
                    stats.unsupported_labels++;
                }
            }
            if (index < payload_size && record[index] == TAL_ANNOTATION_SEP) {
                ++index;
            }
        }
        if (index < payload_size && record[index] == 0) ++index;
    }
    return EdfReportEventStatus::Ok;
}

}  // namespace aircannect
