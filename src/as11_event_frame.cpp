#include "as11_event_frame.h"

namespace aircannect {
namespace {

const char *const AS11_EVENT_NAME_VALUE_CHANGE = "ValueChange";

}  // namespace

bool as11_event_data_id_is_activity(const std::string &data_id) {
    return data_id == "SystemActivityEvents-FrequentActivityEvents" ||
           data_id == "SystemActivityEvents-SporadicActivityEvents";
}

As11EventRecordKind as11_event_record_kind_from_name(
    const std::string &name) {
    if (name == AS11_EVENT_NAME_VALUE_CHANGE) {
        return As11EventRecordKind::ValueChange;
    }
    return As11EventRecordKind::Named;
}

bool as11_event_record_is_value_change(const As11EventRecord &record) {
    return record.kind == As11EventRecordKind::ValueChange;
}

bool as11_event_record_value_change(const As11EventRecord &record,
                                    int32_t &value) {
    if (!as11_event_record_is_value_change(record) || !record.has_value) {
        return false;
    }
    value = record.value;
    return true;
}

}  // namespace aircannect
