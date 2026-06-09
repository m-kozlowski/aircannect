#include "as11_event_frame.h"

namespace aircannect {

bool as11_event_data_id_is_activity(const std::string &data_id) {
    return data_id == "SystemActivityEvents-FrequentActivityEvents" ||
           data_id == "SystemActivityEvents-SporadicActivityEvents";
}

}  // namespace aircannect
