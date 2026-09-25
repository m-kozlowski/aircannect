#include "display_device.h"

#include "board_display.h"

#if AC_DISPLAY_DRIVER == AC_DISPLAY_DRIVER_ST7789

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "memory_manager.h"

namespace aircannect {
namespace {

static_assert(AC_DISPLAY_DC_GPIO >= 0,
              "ST7789 display requires a DC pin");
static_assert(AC_DISPLAY_CS_GPIO >= 0,
              "ST7789 display requires a CS pin");
static_assert(AC_DISPLAY_SCK_GPIO >= 0,
              "ST7789 display requires an SCK pin");
static_assert(AC_DISPLAY_MOSI_GPIO >= 0,
              "ST7789 display requires a MOSI pin");
static_assert(AC_DISPLAY_RESET_GPIO >= 0,
              "ST7789 display requires a reset pin");
static_assert(AC_DISPLAY_BACKLIGHT_GPIO >= 0,
              "ST7789 display requires a backlight pin");

class PsramCanvas : public Arduino_Canvas {
public:
    PsramCanvas(int16_t width, int16_t height, Arduino_G *output)
        : Arduino_Canvas(width, height, output) {}

    bool reserve() {
        if (_framebuffer) return true;

        const size_t bytes = static_cast<size_t>(_width) *
                             static_cast<size_t>(_height) * sizeof(uint16_t);
        _framebuffer = static_cast<uint16_t *>(
            Memory::alloc_large(bytes, false));
        return _framebuffer != nullptr;
    }
};

class St7789DisplayDevice final : public DisplayDevice {
public:
    St7789DisplayDevice()
        : bus_(AC_DISPLAY_DC_GPIO, AC_DISPLAY_CS_GPIO,
               AC_DISPLAY_SCK_GPIO, AC_DISPLAY_MOSI_GPIO,
               AC_DISPLAY_MISO_GPIO),
          panel_(&bus_, AC_DISPLAY_RESET_GPIO,
                 AC_DISPLAY_ROTATION, true,
                 AC_DISPLAY_WIDTH, AC_DISPLAY_HEIGHT),
          canvas_(AC_DISPLAY_WIDTH, AC_DISPLAY_HEIGHT, &panel_) {}

    bool begin() override {
        pinMode(AC_DISPLAY_BACKLIGHT_GPIO, OUTPUT);
        set_backlight(false);

        if (!canvas_.reserve() || !canvas_.begin(AC_DISPLAY_SPI_HZ)) {
            return false;
        }

        canvas_.fillScreen(0);
        canvas_.flush(true);
        return true;
    }

    int16_t width() const override { return canvas_.width(); }
    int16_t height() const override { return canvas_.height(); }

    void set_backlight(bool enabled) override {
        const bool high = AC_DISPLAY_BACKLIGHT_ACTIVE_HIGH
                              ? enabled
                              : !enabled;
        digitalWrite(AC_DISPLAY_BACKLIGHT_GPIO, high ? HIGH : LOW);
    }

    void set_rotation(uint8_t rotation) override {
        canvas_.setRotation(rotation & 0x03u);
    }

    void fill(uint16_t color) override {
        canvas_.fillScreen(color);
    }

    void fill_rect(int16_t x, int16_t y,
                   int16_t width, int16_t height,
                   uint16_t color) override {
        canvas_.fillRect(x, y, width, height, color);
    }

    void draw_text(int16_t x, int16_t y,
                   const char *text,
                   uint16_t color,
                   uint8_t size) override {
        canvas_.setTextWrap(false);
        canvas_.setTextSize(size);
        canvas_.setTextColor(color);
        canvas_.setCursor(x, y);
        canvas_.print(text ? text : "");
    }

    void flush_rect(int16_t x, int16_t y,
                    int16_t width, int16_t height) override {
        if (width <= 0 || height <= 0) return;

        const int32_t logical_width = canvas_.width();
        const int32_t logical_height = canvas_.height();
        int32_t x0 = x;
        int32_t y0 = y;
        int32_t x1 = x0 + width;
        int32_t y1 = y0 + height;

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > logical_width) x1 = logical_width;
        if (y1 > logical_height) y1 = logical_height;
        if (x0 >= x1 || y0 >= y1) return;

        const int32_t framebuffer_width = AC_DISPLAY_WIDTH;
        const int32_t framebuffer_height = AC_DISPLAY_HEIGHT;
        int32_t framebuffer_x = x0;
        int32_t framebuffer_y = y0;
        int32_t framebuffer_rect_width = x1 - x0;
        int32_t framebuffer_rect_height = y1 - y0;

        switch (canvas_.getRotation() & 0x03u) {
        case 1:
            framebuffer_x = framebuffer_width - y1;
            framebuffer_y = x0;
            framebuffer_rect_width = y1 - y0;
            framebuffer_rect_height = x1 - x0;
            break;
        case 2:
            framebuffer_x = framebuffer_width - x1;
            framebuffer_y = framebuffer_height - y1;
            break;
        case 3:
            framebuffer_x = y0;
            framebuffer_y = framebuffer_height - x1;
            framebuffer_rect_width = y1 - y0;
            framebuffer_rect_height = x1 - x0;
            break;
        default:
            break;
        }

        uint16_t *framebuffer = canvas_.getFramebuffer();
        if (!framebuffer) return;

        panel_.startWrite();
        panel_.writeAddrWindow(
            static_cast<int16_t>(framebuffer_x),
            static_cast<int16_t>(framebuffer_y),
            static_cast<uint16_t>(framebuffer_rect_width),
            static_cast<uint16_t>(framebuffer_rect_height));

        // Arduino_ESP32SPI reads pairs even when passed an odd pixel count.
        const auto write_pixels = [this](uint16_t *pixels,
                                         uint32_t count) {
            const uint32_t even_count = count & ~1u;
            if (even_count) panel_.writePixels(pixels, even_count);
            if (even_count != count) panel_.writeColor(pixels[even_count]);
        };

        uint16_t *row = framebuffer +
                        framebuffer_y * framebuffer_width + framebuffer_x;
        if (framebuffer_rect_width == framebuffer_width) {
            write_pixels(
                row,
                static_cast<uint32_t>(framebuffer_rect_width) *
                    framebuffer_rect_height);
        } else {
            for (int32_t row_index = 0;
                 row_index < framebuffer_rect_height; ++row_index) {
                write_pixels(row, framebuffer_rect_width);
                row += framebuffer_width;
            }
        }

        panel_.endWrite();
    }

    void flush() override {
        canvas_.flush(true);
    }

private:
    Arduino_ESP32SPI bus_;
    Arduino_ST7789 panel_;
    PsramCanvas canvas_;
};

}  // namespace

DisplayDevice *board_display_device() {
    static St7789DisplayDevice display;
    return &display;
}

}  // namespace aircannect

#else

namespace aircannect {

DisplayDevice *board_display_device() {
    return nullptr;
}

}  // namespace aircannect

#endif
