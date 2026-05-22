#pragma once

#include "oximetry_manager.h"

#include <stdint.h>
#include <stddef.h>

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

static constexpr uint16_t PLX_SFLOAT_NAN = 0x07ff;
static constexpr uint16_t PLX_SFLOAT_NRES = 0x0800;
static constexpr uint16_t PLX_SFLOAT_POS_INF = 0x07fe;
static constexpr uint16_t PLX_SFLOAT_NEG_INF = 0x0802;
static constexpr uint16_t PLX_SFLOAT_RESERVED = 0x0801;
static constexpr uint16_t BLE_CONN_NONE = 0xffff;

static constexpr const char *SENSOR_NS = "oxi_sensor";
static constexpr const char *SENSOR_KNOWN_COUNT_KEY = "known_count";

static constexpr const char *PLX_SERVICE_UUID = "1822";
static constexpr const char *PLX_CONTINUOUS_UUID = "2A5F";
static constexpr const char *PLX_SPOT_UUID = "2A5E";
static constexpr const char *NONIN_SERVICE_UUID =
    "46A970E0-0D5F-11E2-8B5E-0002A5D5C51B";
static constexpr const char *NONIN_CONTINUOUS_UUID =
    "0AAD7EA0-0D60-11E2-8E3C-0002A5D5C51B";
static constexpr const char *VIATOM_SERVICE_UUID =
    "14839AC4-7D7E-415C-9A42-167340CF2339";
static constexpr const char *VIATOM_READ_UUID =
    "0734594A-A8E7-4B1A-A6B1-CD5243059A57";
static constexpr const char *VIATOM_WRITE_UUID =
    "8B00ACE7-EB0B-49B0-BBE9-9AEE0A26E1A3";

uint16_t encode_sfloat_int_value(int16_t value);
uint8_t crc8_ccitt(const uint8_t *data,
                   size_t len,
                   uint8_t crc = 0x00);

#if AC_OXIMETRY_BLE_ENABLED
extern NimBLEServer *ble_server;
extern NimBLECharacteristic *plx_continuous;
extern NimBLECharacteristic *plx_features;
extern uint16_t ble_conn_handle;
extern NimBLEClient *sensor_client;
extern SemaphoreHandle_t ble_runtime_mutex;
extern OximetryManager *sensor_owner;

bool ensure_ble_runtime(const char *name);
#endif

}  // namespace aircannect
