#include "ble_sensor_protocols.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <algorithm>
#include <cstring>

#include "debug_log.h"
#include "little_endian.h"
#include "oximetry_codec.h"

namespace aircannect {

bool BleSensorProtocolEngine::pc60fw_name(const char *name) {
    if (!name) return false;
    for (const char *model : {"POD-1", "POD-1W", "S5W", "PC-60FW"}) {
        const size_t n = strlen(model);
        if (!strncmp(name, model, n) &&
            (!name[n] || name[n] == '_' || name[n] == '-' || name[n] == ' ')) {
            return true;
        }
    }
    return false;
}

bool BleSensorProtocolEngine::subscribe_pc60fw() {
    auto *service = client_->getService(NimBLEUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
    if (!service) return false;

    auto *tx = service->getCharacteristic(NimBLEUUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"));
    if (!tx || !tx->canNotify()) return false;

    const auto notify = [](NimBLERemoteCharacteristic *ch, uint8_t *p, size_t n, bool) {
        if (auto *engine = active_.load(std::memory_order_acquire)) {
            engine->enqueue_notification(ActiveProtocol::Pc60fw, ch, p, n);
        }
    };
    if (!tx->subscribe(true, notify)) return false;

    notify_characteristic_.store(tx, std::memory_order_release);
    Log::logf(CAT_OXI, LOG_DEBUG, "Sensor subscribed PC-60FW live\n");
    return true;
}

static uint8_t crc8_maxim(const uint8_t *p, size_t size) {
    uint8_t crc = 0;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8c : 0);
        }
    }
    return crc;
}

void BleSensorProtocolEngine::pc60fw_notify(const uint8_t *data, size_t len) {
    while (len) {
        const size_t n = std::min(len, sizeof(pc60fw_rx_) - pc60fw_used_);
        memcpy(pc60fw_rx_ + pc60fw_used_, data, n);
        pc60fw_used_ += n;
        data += n;
        len -= n;

        size_t pos = 0;
        while (pos < pc60fw_used_) {
            const uint8_t *f = pc60fw_rx_ + pos;
            const size_t left = pc60fw_used_ - pos;
            if (f[0] != 0xaa || (left >= 2 && f[1] != 0x55)) {
                ++pos;
                continue;
            }
            if (left < 4) break;

            const size_t size = 4u + f[3]; // Length includes function and CRC.
            if (left < size) break;
            if (f[3] < 2 || crc8_maxim(f, size) != 0) {
                ++pos;
                continue;
            }
            pos += size;

            // RtParam only; waveform and device-status frames carry no vitals.
            if (f[2] != 0x0f || f[4] != 0x01 || f[3] < 8) continue;
            const uint16_t pulse = LittleEndian::get_le16(f + 6);
            const bool has_o2 = f[5] > 0 && f[5] <= 100;
            const bool has_pulse = pulse > 0 && pulse <= 511;
            const bool worn = has_o2 || has_pulse || !(f[9] & 0x02);

            emit_sample(has_o2 ? f[5] : PLX_SFLOAT_NAN,
                        has_pulse ? pulse : PLX_SFLOAT_NAN,
                        !(has_o2 && has_pulse), true, worn, false,
                        f[8] ? f[8] / 10.0f : -1.0f);
        }

        pc60fw_used_ -= pos;
        if (pos && pc60fw_used_) memmove(pc60fw_rx_, pc60fw_rx_ + pos, pc60fw_used_);
    }
}

}  // namespace aircannect
#endif
