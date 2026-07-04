#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_parser.h"
#include "report_spool_types.h"

namespace aircannect {

class ReportSeriesBitReader {
public:
    ReportSeriesBitReader(const uint8_t *data, size_t len);

    bool read_bit(uint8_t &out);

private:
    const uint8_t *data_ = nullptr;
    size_t len_ = 0;
    size_t byte_index_ = 0;
    uint8_t bit_index_ = 0;
};

void report_series_set_error(char *error,
                             size_t error_len,
                             const char *message);

int64_t report_series_zigzag_decode(uint64_t value);
bool report_series_power_of_two_positive(int32_t value);
bool report_series_read_rice(ReportSeriesBitReader &bits,
                             int32_t modulus,
                             uint64_t &out);

int16_t report_series_read_le_i16(const uint8_t *data);
void report_series_put_le32(uint8_t *out, uint32_t value);
int32_t report_series_milli_from_scaled(int64_t raw, double scale);

bool report_series_append_scaled_value_le(ReportSpoolBuffer &values,
                                          int64_t raw,
                                          double scale);

bool report_parse_therapy_1minute_spool(const ReportSpoolResult &result,
                                        ReportParsedChunkCallback callback,
                                        void *context,
                                        char *error,
                                        size_t error_len);

}  // namespace aircannect
