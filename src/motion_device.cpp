#include "motion_device.h"

#include "board_motion.h"

#if AC_MOTION_DRIVER == AC_MOTION_DRIVER_QMI8658

#include <Arduino.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <string.h>

#include "debug_log.h"
#include "shared_i2c_bus.h"

namespace aircannect {
namespace {

static_assert(AC_MOTION_I2C_SDA_GPIO >= 0,
              "QMI8658 requires an I2C SDA pin");
static_assert(AC_MOTION_I2C_SCL_GPIO >= 0,
              "QMI8658 requires an I2C SCL pin");

constexpr uint8_t MOTION_INIT_ATTEMPTS = 3;
constexpr uint32_t MOTION_INIT_RETRY_MS = 50;
constexpr uint8_t MOTION_IO_FAILURES_BEFORE_REPORT = 5;
constexpr uint32_t MOTION_FAULT_PROBE_MS = 1000;
constexpr float MOTION_MIN_VECTOR_G = 0.25f;

constexpr uint8_t QMI8658_WHO_AM_I = 0x00;
constexpr uint8_t QMI8658_CTRL1 = 0x02;
constexpr uint8_t QMI8658_CTRL2 = 0x03;
constexpr uint8_t QMI8658_CTRL5 = 0x06;
constexpr uint8_t QMI8658_CTRL7 = 0x08;
constexpr uint8_t QMI8658_TIMESTAMP_L = 0x30;
constexpr uint8_t QMI8658_AX_L = 0x35;
constexpr uint8_t QMI8658_RESET = 0x60;
constexpr uint8_t QMI8658_RESET_RESULT = 0x4D;

constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;
constexpr uint8_t QMI8658_RESET_VALUE = 0xB0;
constexpr uint8_t QMI8658_RESET_DONE = 0x80;
constexpr uint8_t QMI8658_CTRL1_SINGLE_REGISTER = 0x00;
constexpr uint8_t QMI8658_CTRL2_4G_125HZ = 0x16;
constexpr uint8_t QMI8658_CTRL5_ACCEL_LPF_MODE_0 = 0x01;
constexpr uint8_t QMI8658_CTRL7_ACCEL_ENABLE = 0x01;

constexpr uint32_t QMI8658_I2C_HZ = 400000;
constexpr uint32_t QMI8658_SCL_WAIT_US = 20000;
constexpr uint32_t QMI8658_RESET_SETTLE_MS = 100;
constexpr uint32_t QMI8658_RESET_TIMEOUT_MS = 500;
constexpr uint32_t QMI8658_SAMPLE_TIMEOUT_MS = 100;
constexpr uint32_t QMI8658_STALE_DATA_TIMEOUT_MS = 2000;
constexpr uint32_t QMI8658_TRANSACTION_TIMEOUT_MS = 50;
constexpr float QMI8658_ACCEL_SCALE_G = 4.0f / 32768.0f;
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
        const uint32_t now_ms = millis();
        if (next_probe_ms_ != 0 &&
            static_cast<int32_t>(next_probe_ms_ - now_ms) > 0) {
            return false;
        }

        const ReadResult result = read_sample(sample);

        switch (result) {
            case ReadResult::Ready:
                note_sample_available(now_ms);
                return true;
            case ReadResult::InvalidSample:
                consecutive_io_failures_ = 0;
                schedule_fault_probe(now_ms);
                return false;
            case ReadResult::NoData:
                consecutive_io_failures_ = 0;
                schedule_fault_probe(now_ms);
                return false;
            case ReadResult::StaleData:
                consecutive_io_failures_ = 0;
                enter_stale_fault(now_ms);
                return false;
            case ReadResult::IoError:
                note_i2c_failure(now_ms);
                return false;
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

    void note_sample_available(uint32_t now_ms) {
        consecutive_io_failures_ = 0;
        next_probe_ms_ = 0;

        if (fault_active_) {
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[DISPLAY][MOTION] sensor data resumed "
                      "elapsed_ms=%lu\n",
                      static_cast<unsigned long>(
                          now_ms - fault_started_ms_));
        }

        fault_started_ms_ = 0;
        fault_active_ = false;
    }

    bool begin_fault(uint32_t now_ms) {
        next_probe_ms_ = now_ms + MOTION_FAULT_PROBE_MS;
        if (fault_active_) return false;

        fault_active_ = true;
        fault_started_ms_ = now_ms;
        return true;
    }

    void note_i2c_failure(uint32_t now_ms) {
        if (consecutive_io_failures_ < UINT8_MAX) {
            ++consecutive_io_failures_;
        }
        if (consecutive_io_failures_ <
            MOTION_IO_FAILURES_BEFORE_REPORT) {
            schedule_fault_probe(now_ms);
            return;
        }

        if (begin_fault(now_ms)) {
            Log::logf(
                CAT_GENERAL, LOG_WARN,
                "[DISPLAY][MOTION] I2C communication failed "
                "error=%s(%d); probing for recovery\n",
                esp_err_to_name(last_i2c_error_),
                static_cast<int>(last_i2c_error_));
        }
    }

    void enter_stale_fault(uint32_t now_ms) {
        if (!begin_fault(now_ms)) return;

        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[DISPLAY][MOTION] sample timestamp stopped "
                  "timestamp=0x%06lx; probing for recovery\n",
                  static_cast<unsigned long>(sample_timestamp_));
    }

    void schedule_fault_probe(uint32_t now_ms) {
        if (fault_active_) {
            next_probe_ms_ = now_ms + MOTION_FAULT_PROBE_MS;
        }
    }

    bool start_bus() {
        i2c_master_bus_handle_t bus = board_shared_i2c_bus();
        if (!bus) return false;

        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = AC_MOTION_I2C_ADDRESS;
        device_config.scl_speed_hz = QMI8658_I2C_HZ;
        device_config.scl_wait_us = QMI8658_SCL_WAIT_US;
        device_config.flags.disable_ack_check = false;

        if (i2c_master_bus_add_device(bus, &device_config, &device_) !=
            ESP_OK) {
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
    }

    bool initialize() {
        consecutive_io_failures_ = 0;
        fault_started_ms_ = 0;
        next_probe_ms_ = 0;
        fault_active_ = false;
        sample_timestamp_ = 0;
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

        if (!write_register(QMI8658_CTRL7, 0) ||
            !write_register(QMI8658_CTRL2,
                            QMI8658_CTRL2_4G_125HZ) ||
            !write_register(QMI8658_CTRL5,
                            QMI8658_CTRL5_ACCEL_LPF_MODE_0) ||
            !write_register(QMI8658_CTRL1,
                            QMI8658_CTRL1_SINGLE_REGISTER) ||
            !write_register(QMI8658_CTRL7,
                            QMI8658_CTRL7_ACCEL_ENABLE)) {
            return false;
        }

        if (!configuration_matches()) return false;

        const uint32_t sample_started_ms = millis();
        MotionSample sample;
        do {
            if (read_sample(sample) == ReadResult::Ready) return true;
            delay(5);
        } while (millis() - sample_started_ms < QMI8658_SAMPLE_TIMEOUT_MS);

        return false;
    }

    bool configuration_matches() {
        uint8_t ctrl1 = 0;
        uint8_t ctrl2 = 0;
        uint8_t ctrl5 = 0;
        uint8_t ctrl7 = 0;
        const bool read_ok =
            read_register(QMI8658_CTRL1, &ctrl1, 1) &&
            read_register(QMI8658_CTRL2, &ctrl2, 1) &&
            read_register(QMI8658_CTRL5, &ctrl5, 1) &&
            read_register(QMI8658_CTRL7, &ctrl7, 1);
        const bool matches =
            read_ok &&
            ctrl1 == QMI8658_CTRL1_SINGLE_REGISTER &&
            ctrl2 == QMI8658_CTRL2_4G_125HZ &&
            ctrl5 == QMI8658_CTRL5_ACCEL_LPF_MODE_0 &&
            ctrl7 == QMI8658_CTRL7_ACCEL_ENABLE;
        if (matches) return true;

        Log::logf(
            CAT_GENERAL, LOG_WARN,
            "[DISPLAY][MOTION] configuration readback failed "
            "read_ok=%s ctrl1=0x%02x ctrl2=0x%02x ctrl5=0x%02x "
            "ctrl7=0x%02x error=%s(%d)\n",
            read_ok ? "yes" : "no", ctrl1, ctrl2, ctrl5, ctrl7,
            esp_err_to_name(last_i2c_error_),
            static_cast<int>(last_i2c_error_));
        return false;
    }

    ReadResult read_sample(MotionSample &sample) {
        uint8_t timestamp[QMI8658_TIMESTAMP_BYTES] = {};
        if (!read_register(QMI8658_TIMESTAMP_L, timestamp,
                           sizeof(timestamp))) {
            return ReadResult::IoError;
        }

        const uint32_t now_ms = millis();
        const uint32_t sample_timestamp =
            static_cast<uint32_t>(timestamp[0]) |
            static_cast<uint32_t>(timestamp[1]) << 8 |
            static_cast<uint32_t>(timestamp[2]) << 16;

        if (have_sample_timestamp_ &&
            sample_timestamp == sample_timestamp_) {
            return now_ms - last_timestamp_change_ms_ >=
                           QMI8658_STALE_DATA_TIMEOUT_MS
                       ? ReadResult::StaleData
                       : ReadResult::NoData;
        }
        sample_timestamp_ = sample_timestamp;
        last_timestamp_change_ms_ = now_ms;
        have_sample_timestamp_ = true;

        uint8_t axes[QMI8658_SAMPLE_BYTES] = {};
        if (!read_register(QMI8658_AX_L, axes, sizeof(axes))) {
            return ReadResult::IoError;
        }

        float x = decode_axis(axes) * QMI8658_ACCEL_SCALE_G;
        float y = decode_axis(axes + 2) * QMI8658_ACCEL_SCALE_G;
        float z = decode_axis(axes + 4) * QMI8658_ACCEL_SCALE_G;

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

    bool write_register(uint8_t reg, uint8_t value) {
        transaction_buffer_[0] = reg;
        transaction_buffer_[1] = value;

        if (!device_) {
            last_i2c_error_ = ESP_ERR_INVALID_STATE;
            return false;
        }

        last_i2c_error_ = i2c_master_transmit(
            device_, transaction_buffer_, 2,
            QMI8658_TRANSACTION_TIMEOUT_MS);
        return last_i2c_error_ == ESP_OK;
    }

    bool read_register(uint8_t reg, uint8_t *data, size_t size) {
        if (!device_ || !data || size > QMI8658_MAX_READ_BYTES) {
            last_i2c_error_ = ESP_ERR_INVALID_ARG;
            return false;
        }

        for (size_t i = 0; i < size; ++i) {
            transaction_buffer_[0] = static_cast<uint8_t>(reg + i);
            last_i2c_error_ = i2c_master_transmit_receive(
                device_, transaction_buffer_, 1,
                transaction_buffer_ + 1, 1,
                QMI8658_TRANSACTION_TIMEOUT_MS);
            if (last_i2c_error_ != ESP_OK) return false;

            data[i] = transaction_buffer_[1];
        }
        return true;
    }

    static int16_t decode_axis(const uint8_t *data) {
        return static_cast<int16_t>(
            static_cast<uint16_t>(data[0]) |
            static_cast<uint16_t>(data[1]) << 8);
    }

    static constexpr size_t QMI8658_SAMPLE_BYTES = 6;
    static constexpr size_t QMI8658_TIMESTAMP_BYTES = 3;
    static constexpr size_t QMI8658_MAX_READ_BYTES = QMI8658_SAMPLE_BYTES;

    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t transaction_buffer_[QMI8658_MAX_READ_BYTES + 1] = {};

    uint8_t consecutive_io_failures_ = 0;
    uint32_t fault_started_ms_ = 0;
    uint32_t next_probe_ms_ = 0;
    uint32_t sample_timestamp_ = 0;
    uint32_t last_timestamp_change_ms_ = 0;
    esp_err_t last_i2c_error_ = ESP_OK;
    bool fault_active_ = false;
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
