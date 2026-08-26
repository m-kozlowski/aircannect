#include "ble_sensor_protocols.h"

#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "oximetry_codec.h"

namespace aircannect {

bool BleSensorProtocolEngine::begin() {
#if AC_OXIMETRY_BLE_ENABLED
    if (notification_queue_) return true;

    notification_queue_ = static_cast<NotificationSlot *>(
        Memory::calloc_large(NotificationQueueCapacity,
                             sizeof(NotificationSlot), false));
    return notification_queue_ != nullptr;
#else
    return true;
#endif
}

void BleSensorProtocolEngine::set_sample_callback(SampleCallback callback,
                                                   void *context) {
    sample_callback_ = callback;
    sample_context_ = context;
}

void BleSensorProtocolEngine::emit_sample(uint16_t spo2_raw,
                                          uint16_t pulse_raw,
                                          bool invalid,
                                          bool contact_known,
                                          bool contact_present,
                                          bool charging) {
    if (!sample_callback_) return;

    sample_callback_(sample_context_, spo2_raw, pulse_raw, invalid,
                     contact_known, contact_present, charging);
}

#if AC_OXIMETRY_BLE_ENABLED
std::atomic<BleSensorProtocolEngine *> BleSensorProtocolEngine::active_ =
    nullptr;

bool BleSensorProtocolEngine::matches(
    const NimBLEAdvertisedDevice *device) const {
    if (!device) return false;

    const std::string name = device->getName();
    bool oxyii_manufacturer = false;
    for (uint8_t i = 0; i < device->getManufacturerDataCount(); ++i) {
        const std::string data = device->getManufacturerData(i);
        if (data.size() < 2) continue;

        const uint16_t company =
            static_cast<uint8_t>(data[0]) |
            (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
        if (company == 0x036f || company == 0xf34e) {
            oxyii_manufacturer = true;
            break;
        }
    }

    return oxyii_manufacturer ||
           device->isAdvertisingService(NimBLEUUID("1822")) ||
           device->isAdvertisingService(NimBLEUUID(
               "46A970E0-0D5F-11E2-8B5E-0002A5D5C51B")) ||
           device->isAdvertisingService(NimBLEUUID(
               "14839AC4-7D7E-415C-9A42-167340CF2339")) ||
           device->isAdvertisingService(NimBLEUUID(
               "E8FB0001-A14B-98F9-831B-4E2941D01248")) ||
           name.rfind("Nonin", 0) == 0 ||
           name.rfind("O2 ", 0) == 0 ||
           name.rfind("O2Ring", 0) == 0 ||
           name.rfind("O2M", 0) == 0 ||
           name.rfind("S8-AW", 0) == 0 ||
           name.rfind("T8520_", 0) == 0 ||
           name.rfind("CheckMe", 0) == 0 ||
           name.rfind("Checkme", 0) == 0 ||
           name.rfind("CheckO2", 0) == 0 ||
           name.rfind("SleepU", 0) == 0 ||
           name.rfind("SleepO2", 0) == 0 ||
           name.rfind("WearO2", 0) == 0 ||
           name.rfind("KidsO2", 0) == 0 ||
           name.rfind("BabyO2", 0) == 0 ||
           name.rfind("OxyLink", 0) == 0 ||
           name.rfind("Oxylink", 0) == 0;
}

bool BleSensorProtocolEngine::subscribe(NimBLEClient *client) {
    reset();
    if (!client || !notification_queue_) return false;

    client_ = client;
    active_.store(this, std::memory_order_release);
    active_protocol_ = ActiveProtocol::Plx;
    if (subscribe_plx()) {
        return true;
    }
    active_protocol_ = ActiveProtocol::Nonin;
    if (subscribe_nonin()) {
        return true;
    }
    active_protocol_ = ActiveProtocol::Viatom;
    if (subscribe_viatom()) {
        return true;
    }
    active_protocol_ = ActiveProtocol::Oxyii;
    if (subscribe_oxyii()) {
        return true;
    }

    reset();
    return false;
}
#endif

void BleSensorProtocolEngine::invalidate_connection() {
#if AC_OXIMETRY_BLE_ENABLED
    BleSensorProtocolEngine *expected = this;
    (void)active_.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
    notify_characteristic_.store(nullptr, std::memory_order_release);
    connection_generation_.fetch_add(1, std::memory_order_acq_rel);
#endif
}

void BleSensorProtocolEngine::on_connected() {
#if AC_OXIMETRY_BLE_ENABLED
    switch (active_protocol_) {
        case ActiveProtocol::Viatom:
            viatom_on_connected();
            break;
        case ActiveProtocol::Oxyii:
            oxyii_on_connected();
            break;
        case ActiveProtocol::None:
        case ActiveProtocol::Plx:
        case ActiveProtocol::Nonin:
            break;
    }
#endif
}

void BleSensorProtocolEngine::reset() {
#if AC_OXIMETRY_BLE_ENABLED
    invalidate_connection();
    viatom_reset();
    oxyii_reset();
    client_ = nullptr;
#endif
    active_protocol_ = ActiveProtocol::None;
}

void BleSensorProtocolEngine::poll(uint32_t now_ms) {
#if AC_OXIMETRY_BLE_ENABLED
    drain_notifications();
    if (!client_ || !client_->isConnected()) return;

    switch (active_protocol_) {
        case ActiveProtocol::Viatom:
            viatom_poll(now_ms);
            break;
        case ActiveProtocol::Oxyii:
            oxyii_poll(now_ms);
            break;
        case ActiveProtocol::None:
        case ActiveProtocol::Plx:
        case ActiveProtocol::Nonin:
            break;
    }
#else
    (void)now_ms;
#endif
}

#if AC_OXIMETRY_BLE_ENABLED
bool BleSensorProtocolEngine::enqueue_notification(
    ActiveProtocol protocol,
    NimBLERemoteCharacteristic *characteristic,
    const uint8_t *data,
    size_t len) {
    const uint32_t generation =
        connection_generation_.load(std::memory_order_acquire);
    if (!notification_queue_ || !data || !len ||
        len > NotificationMaxBytes) {
        notification_fault_generation_.store(generation,
                                             std::memory_order_release);
        notification_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint32_t write =
        notification_write_sequence_.load(std::memory_order_relaxed);
    const uint32_t read =
        notification_read_sequence_.load(std::memory_order_acquire);
    if (write - read >= NotificationQueueCapacity) {
        notification_fault_generation_.store(generation,
                                             std::memory_order_release);
        notification_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    NotificationSlot &slot =
        notification_queue_[write % NotificationQueueCapacity];
    slot.generation = generation;
    slot.protocol = protocol;
    slot.characteristic = characteristic;
    slot.length = static_cast<uint16_t>(len);
    memcpy(slot.bytes, data, len);
    notification_write_sequence_.store(write + 1,
                                       std::memory_order_release);
    return true;
}

void BleSensorProtocolEngine::drain_notifications() {
    if (!notification_queue_) return;

    const uint32_t generation =
        connection_generation_.load(std::memory_order_acquire);
    if (notification_fault_generation_.exchange(
            0, std::memory_order_acq_rel) == generation) {
        reset_fragment_assemblers();
        const uint32_t write =
            notification_write_sequence_.load(std::memory_order_acquire);
        notification_read_sequence_.store(write, std::memory_order_release);
    }

    uint32_t read =
        notification_read_sequence_.load(std::memory_order_relaxed);
    const uint32_t write =
        notification_write_sequence_.load(std::memory_order_acquire);
    while (read != write) {
        const NotificationSlot &notification =
            notification_queue_[read % NotificationQueueCapacity];
        process_notification(notification);
        ++read;
        notification_read_sequence_.store(read, std::memory_order_release);
    }

    const uint32_t drops =
        notification_drops_.load(std::memory_order_relaxed);
    if (drops != reported_notification_drops_) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor BLE notification drops total=%lu\n",
                  static_cast<unsigned long>(drops));
        reported_notification_drops_ = drops;
    }
}

void BleSensorProtocolEngine::process_notification(
    const NotificationSlot &notification) {
    const uint32_t generation =
        connection_generation_.load(std::memory_order_acquire);
    if (notification.generation != generation ||
        notification.protocol != active_protocol_ ||
        notification.characteristic !=
            notify_characteristic_.load(std::memory_order_acquire)) {
        return;
    }

    switch (notification.protocol) {
        case ActiveProtocol::Plx:
            process_plx_notification(notification.bytes,
                                     notification.length);
            break;
        case ActiveProtocol::Nonin:
            process_nonin_notification(notification.bytes,
                                       notification.length);
            break;
        case ActiveProtocol::Viatom:
            viatom_notify(notification.bytes, notification.length);
            break;
        case ActiveProtocol::Oxyii:
            oxyii_notify(notification.bytes, notification.length);
            break;
        case ActiveProtocol::None:
            break;
    }
}

void BleSensorProtocolEngine::reset_fragment_assemblers() {
    viatom_reset_rx();
    oxyii_reset_rx();
}
#endif

}  // namespace aircannect
