#include "motion_device.h"

#include "board_motion.h"

#if AC_MOTION_DRIVER == AC_MOTION_DRIVER_QMI8658

#include <Arduino.h>
#include <driver/i2c_master.h>
#include <esp_rom_sys.h>
#include <string.h>

#include "debug_log.h"

namespace aircannect {
namespace {

static_assert(AC_MOTION_I2C_SDA_GPIO >= 0,
              "QMI8658 requires an I2C SDA pin");
static_assert(AC_MOTION_I2C_SCL_GPIO >= 0,
              "QMI8658 requires an I2C SCL pin");

constexpr uint8_t MOTION_INIT_ATTEMPTS = 3;
constexpr uint32_t MOTION_INIT_RETRY_MS = 50;
constexpr uint8_t MOTION_FAILURES_BEFORE_RECOVERY = 5;
constexpr uint32_t MOTION_RECOVERY_RETRY_MS = 2000;
constexpr float MOTION_MIN_VECTOR_G = 0.25f;

constexpr uint8_t QMI8658_WHO_AM_I = 0x00;
constexpr uint8_t QMI8658_CTRL1 = 0x02;
constexpr uint8_t QMI8658_CTRL2 = 0x03;
constexpr uint8_t QMI8658_CTRL5 = 0x06;
constexpr uint8_t QMI8658_CTRL7 = 0x08;
constexpr uint8_t QMI8658_CTRL9 = 0x0A;
constexpr uint8_t QMI8658_CAL1_L = 0x0B;
constexpr uint8_t QMI8658_STATUSINT = 0x2D;
constexpr uint8_t QMI8658_TIMESTAMP_L = 0x30;
constexpr uint8_t QMI8658_AX_L = 0x35;
constexpr uint8_t QMI8658_RESET = 0x60;
constexpr uint8_t QMI8658_RESET_RESULT = 0x4D;

constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;
constexpr uint8_t QMI8658_RESET_VALUE = 0xB0;
constexpr uint8_t QMI8658_RESET_DONE = 0x80;
constexpr uint8_t QMI8658_CTRL1_AUTO_INCREMENT = 0x40;
constexpr uint8_t QMI8658_CTRL2_4G_125HZ = 0x16;
constexpr uint8_t QMI8658_CTRL5_ACCEL_LPF_MODE_0 = 0x01;
constexpr uint8_t QMI8658_CTRL7_ACCEL_LOCKED = 0x81;
constexpr uint8_t QMI8658_CTRL9_AHB_CLOCK_GATING = 0x12;
constexpr uint8_t QMI8658_CTRL9_ACK = 0x00;
constexpr uint8_t QMI8658_STATUSINT_COMMAND_DONE = 0x80;
constexpr uint8_t QMI8658_STATUSINT_DATA_LOCKED = 0x02;
constexpr uint8_t QMI8658_STATUSINT_DATA_AVAILABLE = 0x01;

constexpr uint32_t QMI8658_I2C_HZ = 400000;
constexpr uint32_t QMI8658_SCL_WAIT_US = 20000;
constexpr uint32_t QMI8658_RESET_SETTLE_MS = 100;
constexpr uint32_t QMI8658_RESET_TIMEOUT_MS = 500;
constexpr uint32_t QMI8658_COMMAND_TIMEOUT_MS = 100;
constexpr uint32_t QMI8658_SAMPLE_TIMEOUT_MS = 100;
constexpr uint32_t QMI8658_STALE_DATA_TIMEOUT_MS = 2000;
constexpr uint32_t QMI8658_TRANSACTION_TIMEOUT_MS = 50;
constexpr uint32_t QMI8658_ACCEL_DATA_READ_DELAY_US = 48;
constexpr float QMI8658_ACCEL_SCALE_G = 4.0f / 32768.0f;
constexpr i2c_port_num_t QMI8658_I2C_PORT = I2C_NUM_0;

class Qmi8658MotionDevice final : public MotionDevice {
public:
    bool begin() override {
        for (uint8_t attempt = 0; attempt < MOTION_INIT_ATTEMPTS; ++attempt) {
            if (attempt > 0) {
                stop_bus();
                delay(MOTION_INIT_RETRY_MS);
            }

            if (!start_bus()) continue;
            if (initialize()) return true;
        }

        stop_bus();
        return false;
    }

    bool read(MotionSample &sample) override {
        const ReadResult result = read_sample(sample);
        if (result == ReadResult::Ready) {
            consecutive_failures_ = 0;
            return true;
        }
        if (result == ReadResult::NoData ||
            result == ReadResult::InvalidSample) {
            consecutive_failures_ = 0;
            return false;
        }

        if (result == ReadResult::StaleData) {
            consecutive_failures_ = MOTION_FAILURES_BEFORE_RECOVERY;
        } else if (consecutive_failures_ < UINT8_MAX) {
            ++consecutive_failures_;
        }
        if (consecutive_failures_ < MOTION_FAILURES_BEFORE_RECOVERY) {
            return false;
        }

        const uint32_t now_ms = millis();
        if (next_recovery_ms_ != 0 &&
            static_cast<int32_t>(next_recovery_ms_ - now_ms) > 0) {
            return false;
        }

        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[DISPLAY][MOTION] sensor stalled reason=%s; "
                  "reinitializing\n",
                  result == ReadResult::StaleData
                      ? "sample_clock_stopped" : "i2c_error");

        if (recover()) {
            consecutive_failures_ = 0;
            next_recovery_ms_ = 0;
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[DISPLAY][MOTION] sensor recovered\n");
        } else {
            next_recovery_ms_ = now_ms + MOTION_RECOVERY_RETRY_MS;
        }
        return false;
    }

private:
    enum class ReadResult : uint8_t {
        Ready,
        NoData,
        StaleData,
        InvalidSample,
        IoError,
    };

    bool start_bus() {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = QMI8658_I2C_PORT;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.sda_io_num =
            static_cast<gpio_num_t>(AC_MOTION_I2C_SDA_GPIO);
        bus_config.scl_io_num =
            static_cast<gpio_num_t>(AC_MOTION_I2C_SCL_GPIO);
        bus_config.glitch_ignore_cnt = 7;
        bus_config.trans_queue_depth = 0;
        bus_config.flags.enable_internal_pullup = true;
        bus_config.flags.allow_pd = false;

        if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) {
            bus_ = nullptr;
            return false;
        }

        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = AC_MOTION_I2C_ADDRESS;
        device_config.scl_speed_hz = QMI8658_I2C_HZ;
        device_config.scl_wait_us = QMI8658_SCL_WAIT_US;
        device_config.flags.disable_ack_check = false;

        if (i2c_master_bus_add_device(bus_, &device_config, &device_) !=
            ESP_OK) {
            (void)i2c_del_master_bus(bus_);
            bus_ = nullptr;
            device_ = nullptr;
            return false;
        }

        return true;
    }

    void stop_bus() {
        if (device_) {
            (void)i2c_master_bus_rm_device(device_);
            device_ = nullptr;
        }
        if (!bus_) return;

        (void)i2c_del_master_bus(bus_);
        bus_ = nullptr;
    }

    bool initialize() {
        last_sample_ms_ = 0;
        last_timestamp_change_ms_ = 0;
        have_sample_timestamp_ = false;

        if (!write_register(QMI8658_RESET, QMI8658_RESET_VALUE)) return false;

        delay(QMI8658_RESET_SETTLE_MS);

        const uint32_t reset_started_ms = millis();
        uint8_t reset_result = 0;
        do {
            if (read_register(QMI8658_RESET_RESULT, &reset_result, 1) &&
                reset_result == QMI8658_RESET_DONE) {
                break;
            }
            delay(10);
        } while (millis() - reset_started_ms < QMI8658_RESET_TIMEOUT_MS);

        if (reset_result != QMI8658_RESET_DONE) return false;

        uint8_t who_am_i = 0;
        if (!read_register(QMI8658_WHO_AM_I, &who_am_i, 1) ||
            who_am_i != QMI8658_WHO_AM_I_VALUE) {
            return false;
        }

        if (!write_register(QMI8658_CTRL1,
                            QMI8658_CTRL1_AUTO_INCREMENT) ||
            !write_register(QMI8658_CTRL7, 0) ||
            !write_register(QMI8658_CTRL2,
                            QMI8658_CTRL2_4G_125HZ) ||
            !write_register(QMI8658_CTRL5,
                            QMI8658_CTRL5_ACCEL_LPF_MODE_0) ||
            !set_ahb_clock_gating(false) ||
            !write_register(QMI8658_CTRL7,
                            QMI8658_CTRL7_ACCEL_LOCKED)) {
            return false;
        }

        const uint32_t sample_started_ms = millis();
        MotionSample sample;
        do {
            if (read_sample(sample) == ReadResult::Ready) return true;
            delay(5);
        } while (millis() - sample_started_ms < QMI8658_SAMPLE_TIMEOUT_MS);

        return false;
    }

    bool set_ahb_clock_gating(bool enabled) {
        if (!write_register(QMI8658_CAL1_L, enabled ? 0x00 : 0x01) ||
            !write_register(QMI8658_CTRL9,
                            QMI8658_CTRL9_AHB_CLOCK_GATING)) {
            return false;
        }

        const uint32_t command_started_ms = millis();
        uint8_t status = 0;
        do {
            if (!read_register(QMI8658_STATUSINT, &status, 1)) return false;
            if (status & QMI8658_STATUSINT_COMMAND_DONE) break;
            delay(1);
        } while (millis() - command_started_ms < QMI8658_COMMAND_TIMEOUT_MS);

        if ((status & QMI8658_STATUSINT_COMMAND_DONE) == 0 ||
            !write_register(QMI8658_CTRL9, QMI8658_CTRL9_ACK)) {
            return false;
        }

        const uint32_t ack_started_ms = millis();
        do {
            if (!read_register(QMI8658_STATUSINT, &status, 1)) return false;
            if ((status & QMI8658_STATUSINT_COMMAND_DONE) == 0) return true;
            delay(1);
        } while (millis() - ack_started_ms < QMI8658_COMMAND_TIMEOUT_MS);

        return false;
    }

    ReadResult read_sample(MotionSample &sample) {
        uint8_t status = 0;
        if (!read_register(QMI8658_STATUSINT, &status, 1)) {
            return ReadResult::IoError;
        }

        const uint32_t now_ms = millis();
        if ((status & QMI8658_STATUSINT_DATA_AVAILABLE) == 0) {
            return last_sample_ms_ != 0 &&
                           now_ms - last_sample_ms_ >=
                               QMI8658_STALE_DATA_TIMEOUT_MS
                       ? ReadResult::StaleData
                       : ReadResult::NoData;
        }

        if ((status & QMI8658_STATUSINT_DATA_LOCKED) == 0) {
            esp_rom_delay_us(QMI8658_ACCEL_DATA_READ_DELAY_US);
        }

        uint8_t data[QMI8658_SAMPLE_BYTES] = {};
        if (!read_register(QMI8658_TIMESTAMP_L, data, sizeof(data))) {
            return ReadResult::IoError;
        }

        const uint32_t sample_timestamp =
            static_cast<uint32_t>(data[0]) |
            static_cast<uint32_t>(data[1]) << 8 |
            static_cast<uint32_t>(data[2]) << 16;
        if (!have_sample_timestamp_ || sample_timestamp != sample_timestamp_) {
            sample_timestamp_ = sample_timestamp;
            last_timestamp_change_ms_ = now_ms;
            have_sample_timestamp_ = true;
        } else if (now_ms - last_timestamp_change_ms_ >=
                   QMI8658_STALE_DATA_TIMEOUT_MS) {
            return ReadResult::StaleData;
        }

        last_sample_ms_ = now_ms;

        float x = decode_axis(data + QMI8658_AXIS_OFFSET) *
                  QMI8658_ACCEL_SCALE_G;
        float y = decode_axis(data + QMI8658_AXIS_OFFSET + 2) *
                  QMI8658_ACCEL_SCALE_G;
        float z = decode_axis(data + QMI8658_AXIS_OFFSET + 4) *
                  QMI8658_ACCEL_SCALE_G;

        if (x * x + y * y + z * z <
            MOTION_MIN_VECTOR_G * MOTION_MIN_VECTOR_G) {
            return ReadResult::InvalidSample;
        }

#if AC_MOTION_SWAP_XY
        const float original_x = x;
        x = y;
        y = original_x;
#endif

#if AC_MOTION_INVERT_X
        x = -x;
#endif

#if AC_MOTION_INVERT_Y
        y = -y;
#endif

        sample.x_g = x;
        sample.y_g = y;
        sample.z_g = z;
        return ReadResult::Ready;
    }

    bool recover() {
        if (bus_ && i2c_master_bus_reset(bus_) != ESP_OK) return false;
        return initialize();
    }

    bool write_register(uint8_t reg, uint8_t value) {
        transaction_buffer_[0] = reg;
        transaction_buffer_[1] = value;

        return device_ &&
               i2c_master_transmit(device_, transaction_buffer_, 2,
                                   QMI8658_TRANSACTION_TIMEOUT_MS) == ESP_OK;
    }

    bool read_register(uint8_t reg, uint8_t *data, size_t size) {
        if (!device_ || !data || size > QMI8658_MAX_READ_BYTES) return false;

        transaction_buffer_[0] = reg;
        const esp_err_t result = i2c_master_transmit_receive(
            device_, transaction_buffer_, 1, transaction_buffer_ + 1,
            size, QMI8658_TRANSACTION_TIMEOUT_MS);
        if (result != ESP_OK) return false;

        memcpy(data, transaction_buffer_ + 1, size);
        return true;
    }

    static int16_t decode_axis(const uint8_t *data) {
        return static_cast<int16_t>(
            static_cast<uint16_t>(data[0]) |
            static_cast<uint16_t>(data[1]) << 8);
    }

    static constexpr size_t QMI8658_AXIS_OFFSET =
        QMI8658_AX_L - QMI8658_TIMESTAMP_L;
    static constexpr size_t QMI8658_SAMPLE_BYTES =
        QMI8658_AXIS_OFFSET + 6;
    static constexpr size_t QMI8658_MAX_READ_BYTES = QMI8658_SAMPLE_BYTES;

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t transaction_buffer_[QMI8658_MAX_READ_BYTES + 1] = {};

    uint8_t consecutive_failures_ = 0;
    uint32_t next_recovery_ms_ = 0;
    uint32_t last_sample_ms_ = 0;
    uint32_t sample_timestamp_ = 0;
    uint32_t last_timestamp_change_ms_ = 0;
    bool have_sample_timestamp_ = false;
};

}  // namespace

MotionDevice *board_motion_device() {
    static Qmi8658MotionDevice motion;
    return &motion;
}

}  // namespace aircannect

#else

namespace aircannect {

MotionDevice *board_motion_device() {
    return nullptr;
}

}  // namespace aircannect

#endif
