#pragma once

#include <stdint.h>

namespace aircannect {

class AudibleOutput {
public:
    virtual ~AudibleOutput() = default;

    virtual bool begin() = 0;
    virtual bool set_volume(uint8_t percent) = 0;
    virtual bool play_tone(uint16_t frequency_hz,
                           uint16_t duration_ms) = 0;
    virtual void silence() = 0;
};

AudibleOutput *board_audible_output();

}  // namespace aircannect
