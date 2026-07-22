#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace aircannect {

static constexpr size_t AC_AS11_EVENT_FRAME_EVENTS_MAX = 16;

enum class As11EventRecordKind {
    Named,
    ValueChange,
};

struct As11EventRecord {
    As11EventRecordKind kind = As11EventRecordKind::Named;
    std::string name;
    std::string report_time;
    int32_t value = 0;
    int32_t duration_ms = 0;
    bool has_value = false;
    bool has_duration = false;
};

struct As11EventFrame {
    uint32_t subscription_id = 0;
    std::string data_id;
    As11EventRecord events[AC_AS11_EVENT_FRAME_EVENTS_MAX];
    size_t event_count = 0;
    bool truncated = false;
};

bool as11_event_data_id_is_activity(const std::string &data_id);
As11EventRecordKind as11_event_record_kind_from_name(const std::string &name);
bool as11_event_record_value_change(const As11EventRecord &record,
                                    int32_t &value);

}  // namespace aircannect
