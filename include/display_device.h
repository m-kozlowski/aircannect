#pragma once

#include <stdint.h>

namespace aircannect {

class DisplayDevice {
public:
    virtual ~DisplayDevice() = default;

    virtual bool begin() = 0;
    virtual int16_t width() const = 0;
    virtual int16_t height() const = 0;

    virtual void set_backlight(bool enabled) = 0;
    virtual void set_rotation(uint8_t rotation) = 0;
    virtual void fill(uint16_t color) = 0;
    virtual void fill_rect(int16_t x, int16_t y,
                           int16_t width, int16_t height,
                           uint16_t color) = 0;
    virtual void draw_text(int16_t x, int16_t y,
                           const char *text,
                           uint16_t color,
                           uint8_t size) = 0;
    virtual void flush() = 0;
};

DisplayDevice *board_display_device();

}  // namespace aircannect
