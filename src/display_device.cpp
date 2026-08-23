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
