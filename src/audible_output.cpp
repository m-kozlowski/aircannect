#include "audible_output.h"

#include "board_audio.h"

#if AC_AUDIO_DRIVER == AC_AUDIO_DRIVER_ES8311_I2S

#include <Arduino.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include "debug_log.h"
#include "shared_i2c_bus.h"

namespace aircannect {
namespace {

static_assert(AC_AUDIO_MCLK_GPIO >= 0, "ES8311 requires an MCLK pin");
static_assert(AC_AUDIO_BCLK_GPIO >= 0, "ES8311 requires a BCLK pin");
static_assert(AC_AUDIO_LRCK_GPIO >= 0, "ES8311 requires an LRCK pin");
static_assert(AC_AUDIO_DOUT_GPIO >= 0, "ES8311 requires a DOUT pin");
static_assert(AC_AUDIO_PA_GPIO >= 0, "ES8311 requires an amplifier pin");
static_assert(AC_AUDIO_SAMPLE_RATE == 16000,
              "ES8311 setup currently supports 16 kHz PCM");

constexpr uint32_t ES8311_I2C_HZ = 400000;
constexpr uint32_t ES8311_I2C_TIMEOUT_MS = 50;
constexpr size_t TONE_FRAMES = 64;
constexpr int16_t TONE_AMPLITUDE = 10000;

uint8_t codec_volume_register(uint8_t percent) {
    if (percent == 0) return 0;
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(percent) * 256U / 100U) - 1U);
}

constexpr uint8_t ES8311_RESET = 0x00;
constexpr uint8_t ES8311_CLOCK1 = 0x01;
constexpr uint8_t ES8311_CLOCK2 = 0x02;
constexpr uint8_t ES8311_CLOCK3 = 0x03;
constexpr uint8_t ES8311_CLOCK4 = 0x04;
constexpr uint8_t ES8311_CLOCK5 = 0x05;
constexpr uint8_t ES8311_CLOCK6 = 0x06;
constexpr uint8_t ES8311_CLOCK7 = 0x07;
constexpr uint8_t ES8311_CLOCK8 = 0x08;
constexpr uint8_t ES8311_DAC_INTERFACE = 0x09;
constexpr uint8_t ES8311_ADC_INTERFACE = 0x0A;
constexpr uint8_t ES8311_SYSTEM0B = 0x0B;
constexpr uint8_t ES8311_SYSTEM0C = 0x0C;
constexpr uint8_t ES8311_SYSTEM0D = 0x0D;
constexpr uint8_t ES8311_SYSTEM0E = 0x0E;
constexpr uint8_t ES8311_SYSTEM10 = 0x10;
constexpr uint8_t ES8311_SYSTEM11 = 0x11;
constexpr uint8_t ES8311_SYSTEM12 = 0x12;
constexpr uint8_t ES8311_SYSTEM13 = 0x13;
constexpr uint8_t ES8311_SYSTEM14 = 0x14;
constexpr uint8_t ES8311_ADC15 = 0x15;
constexpr uint8_t ES8311_ADC16 = 0x16;
constexpr uint8_t ES8311_ADC17 = 0x17;
constexpr uint8_t ES8311_ADC1B = 0x1B;
constexpr uint8_t ES8311_ADC1C = 0x1C;
constexpr uint8_t ES8311_DAC_MUTE = 0x31;
constexpr uint8_t ES8311_DAC_VOLUME = 0x32;
constexpr uint8_t ES8311_DAC_RAMP = 0x37;
constexpr uint8_t ES8311_GPIO44 = 0x44;
constexpr uint8_t ES8311_GPIO45 = 0x45;

class Es8311AudibleOutput final : public AudibleOutput {
public:
    bool begin() override {
        if (initialized_) return true;

        set_amplifier(false);

        if (!initialize_i2s()) {
            Log::logf(CAT_GENERAL, LOG_ERROR,
                      "[AUDIO] I2S initialization failed error=%s(%d)\n",
                      esp_err_to_name(last_error_),
                      static_cast<int>(last_error_));
            return false;
        }

        if (!add_codec() || !initialize_codec()) {
            Log::logf(CAT_GENERAL, LOG_ERROR,
                      "[AUDIO] ES8311 initialization failed error=%s(%d)\n",
                      esp_err_to_name(last_error_),
                      static_cast<int>(last_error_));
            remove_codec();
            shutdown_i2s();
            return false;
        }

        silence();
        initialized_ = true;
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[AUDIO] audible output ready driver=es8311\n");
        return true;
    }

    bool set_volume(uint8_t percent) override {
        if (percent > 100) return false;

        volume_percent_ = percent;
        if (!initialized_) return true;
        if (write_register(ES8311_DAC_VOLUME,
                           codec_volume_register(volume_percent_))) {
            return true;
        }

        log_playback_error("volume_write_failed");
        return false;
    }

    bool play_tone(uint16_t frequency_hz,
                   uint16_t duration_ms) override {
        if (!initialized_ || frequency_hz == 0 || duration_ms == 0) {
            return false;
        }

        if (!set_muted(false)) {
            log_playback_error("codec_unmute_failed");
            return false;
        }

        set_amplifier(true);
        if (AC_AUDIO_PA_STARTUP_MS > 0) {
            delay(AC_AUDIO_PA_STARTUP_MS);
        }

        const uint32_t playback_started_ms = millis();
        uint32_t phase = 0;
        uint32_t frames_left =
            static_cast<uint32_t>(AC_AUDIO_SAMPLE_RATE) * duration_ms / 1000;
        while (frames_left > 0) {
            const size_t frames =
                frames_left < TONE_FRAMES ? frames_left : TONE_FRAMES;
            fill_tone(frequency_hz, phase, frames);

            const size_t bytes = frames * 2 * sizeof(int16_t);
            size_t written = 0;
            last_error_ = i2s_channel_write(
                tx_channel_, samples_, bytes, &written, 1000);
            if (last_error_ != ESP_OK || written != bytes) {
                log_playback_error("i2s_write_failed");
                silence();
                return false;
            }
            frames_left -= frames;
        }

        const uint32_t playback_elapsed_ms = millis() - playback_started_ms;
        if (playback_elapsed_ms < duration_ms) {
            delay(duration_ms - playback_elapsed_ms);
        }

        silence();
        return true;
    }

    void silence() override {
        if (device_) (void)set_muted(true);
        set_amplifier(false);
    }

private:
    bool initialize_i2s() {
        i2s_chan_config_t channel_config =
            I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        last_error_ = i2s_new_channel(
            &channel_config, &tx_channel_, nullptr);
        if (last_error_ != ESP_OK) return false;

        i2s_std_config_t config = {};
        config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AC_AUDIO_SAMPLE_RATE);
        config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        config.gpio_cfg.mclk = static_cast<gpio_num_t>(AC_AUDIO_MCLK_GPIO);
        config.gpio_cfg.bclk = static_cast<gpio_num_t>(AC_AUDIO_BCLK_GPIO);
        config.gpio_cfg.ws = static_cast<gpio_num_t>(AC_AUDIO_LRCK_GPIO);
        config.gpio_cfg.dout = static_cast<gpio_num_t>(AC_AUDIO_DOUT_GPIO);
        config.gpio_cfg.din = I2S_GPIO_UNUSED;
        config.gpio_cfg.invert_flags.mclk_inv = false;
        config.gpio_cfg.invert_flags.bclk_inv = false;
        config.gpio_cfg.invert_flags.ws_inv = false;

        last_error_ = i2s_channel_init_std_mode(tx_channel_, &config);
        if (last_error_ == ESP_OK) {
            last_error_ = i2s_channel_enable(tx_channel_);
        }
        if (last_error_ == ESP_OK) return true;

        shutdown_i2s();
        return false;
    }

    void shutdown_i2s() {
        if (!tx_channel_) return;

        (void)i2s_channel_disable(tx_channel_);
        (void)i2s_del_channel(tx_channel_);
        tx_channel_ = nullptr;
    }

    bool add_codec() {
        i2c_master_bus_handle_t bus = board_shared_i2c_bus();
        if (!bus) {
            last_error_ = ESP_ERR_INVALID_STATE;
            return false;
        }

        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = AC_AUDIO_I2C_ADDRESS;
        config.scl_speed_hz = ES8311_I2C_HZ;
        config.scl_wait_us = 20000;
        config.flags.disable_ack_check = false;

        last_error_ = i2c_master_bus_add_device(bus, &config, &device_);
        return last_error_ == ESP_OK;
    }

    void remove_codec() {
        if (!device_) return;

        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }

    bool initialize_codec() {
        // Sequence follows Espressif's ES8311 driver for 16 kHz, MCLK=256fs.
        const uint8_t setup[][2] = {
            {ES8311_GPIO44, 0x08},
            {ES8311_GPIO44, 0x08},
            {ES8311_CLOCK1, 0x30},
            {ES8311_CLOCK2, 0x00},
            {ES8311_CLOCK3, 0x10},
            {ES8311_ADC16, 0x24},
            {ES8311_CLOCK4, 0x20},
            {ES8311_CLOCK5, 0x00},
            {ES8311_SYSTEM0B, 0x00},
            {ES8311_SYSTEM0C, 0x00},
            {ES8311_SYSTEM10, 0x1F},
            {ES8311_SYSTEM11, 0x7F},
            {ES8311_RESET, 0x80},
            {ES8311_CLOCK1, 0x3F},
            {ES8311_CLOCK6, 0x03},
            {ES8311_CLOCK7, 0x00},
            {ES8311_CLOCK8, 0xFF},
            {ES8311_DAC_INTERFACE, 0x0C},
            {ES8311_ADC_INTERFACE, 0x0C},
            {ES8311_SYSTEM13, 0x10},
            {ES8311_ADC1B, 0x0A},
            {ES8311_ADC1C, 0x6A},
            {ES8311_GPIO44, 0x58},
            {ES8311_ADC17, 0xBF},
            {ES8311_SYSTEM0E, 0x02},
            {ES8311_SYSTEM12, 0x00},
            {ES8311_SYSTEM14, 0x1A},
            {ES8311_SYSTEM0D, 0x01},
            {ES8311_ADC15, 0x40},
            {ES8311_DAC_RAMP, 0x08},
            {ES8311_GPIO45, 0x00},
            {ES8311_DAC_VOLUME,
             codec_volume_register(volume_percent_)},
        };

        for (const auto &entry : setup) {
            if (!write_register(entry[0], entry[1])) return false;
        }
        return set_muted(true);
    }

    bool set_muted(bool muted) {
        return write_register(ES8311_DAC_MUTE, muted ? 0x60 : 0x00);
    }

    bool write_register(uint8_t reg, uint8_t value) {
        if (!device_) {
            last_error_ = ESP_ERR_INVALID_STATE;
            return false;
        }

        const uint8_t bytes[] = {reg, value};
        last_error_ = i2c_master_transmit(
            device_, bytes, sizeof(bytes), ES8311_I2C_TIMEOUT_MS);
        return last_error_ == ESP_OK;
    }

    void log_playback_error(const char *reason) const {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[AUDIO] playback failed reason=%s error=%s(%d)\n",
                  reason, esp_err_to_name(last_error_),
                  static_cast<int>(last_error_));
    }

    void set_amplifier(bool enabled) {
        pinMode(AC_AUDIO_PA_GPIO, OUTPUT);
        const bool level = AC_AUDIO_PA_ACTIVE_HIGH ? enabled : !enabled;
        digitalWrite(AC_AUDIO_PA_GPIO, level ? HIGH : LOW);
    }

    void fill_tone(uint16_t frequency_hz,
                   uint32_t &phase,
                   size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            phase += frequency_hz;
            while (phase >= AC_AUDIO_SAMPLE_RATE) {
                phase -= AC_AUDIO_SAMPLE_RATE;
            }

            const uint32_t half_rate = AC_AUDIO_SAMPLE_RATE / 2;
            const int32_t triangle =
                phase < half_rate
                    ? static_cast<int32_t>(phase * 4) - AC_AUDIO_SAMPLE_RATE
                    : static_cast<int32_t>(AC_AUDIO_SAMPLE_RATE * 3) -
                          static_cast<int32_t>(phase * 4);
            const int16_t sample = static_cast<int16_t>(
                triangle * TONE_AMPLITUDE / AC_AUDIO_SAMPLE_RATE);
            samples_[i * 2] = sample;
            samples_[i * 2 + 1] = sample;
        }
    }

    i2s_chan_handle_t tx_channel_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    int16_t samples_[TONE_FRAMES * 2] = {};
    esp_err_t last_error_ = ESP_OK;
    uint8_t volume_percent_ = 100;
    bool initialized_ = false;
};

}  // namespace

AudibleOutput *board_audible_output() {
    static Es8311AudibleOutput output;
    return &output;
}

}  // namespace aircannect

#else

namespace aircannect {

AudibleOutput *board_audible_output() {
    return nullptr;
}

}  // namespace aircannect

#endif
