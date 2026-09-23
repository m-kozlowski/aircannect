#pragma once

#include <stdint.h>
#include <string>
#include <string_view>

#include "ncp_record.h"
#include "rpc_payload.h"

namespace aircannect {

// Zero means that the method stays on the JSON lane.
uint8_t airmini_ncp_command(std::string_view method);
bool encode_airmini_ncp_rpc(uint8_t command, uint8_t tag,
                            const std::string &params, std::string &record);
RpcPayloadRef decode_airmini_ncp_rpc(const NcpRecordView &record,
                                     uint32_t request_id);

}  // namespace aircannect
