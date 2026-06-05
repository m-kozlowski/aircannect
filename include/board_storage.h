#pragma once

// SD storage: card config, provisioning paths, write-queue runtime, sink.

#include <stddef.h>
#include <stdint.h>

#ifndef AC_STORAGE_SDMMC_ENABLED
#define AC_STORAGE_SDMMC_ENABLED 0
#endif

#ifndef AC_STORAGE_SPI_SD_ENABLED
#define AC_STORAGE_SPI_SD_ENABLED 0
#endif

#ifndef AC_STORAGE_MOUNT_POINT
#define AC_STORAGE_MOUNT_POINT "/sdcard"
#endif

#ifndef AC_STORAGE_MAX_OPEN_FILES
#define AC_STORAGE_MAX_OPEN_FILES 4
#endif

#ifndef AC_PROVISION_CONFIG_PATH
#define AC_PROVISION_CONFIG_PATH "/config.txt"
#endif

#ifndef AC_PROVISION_OK_PATH
#define AC_PROVISION_OK_PATH "/config.ok"
#endif

static constexpr size_t AC_PROVISION_LINE_MAX = 256;

#ifndef AC_SDMMC_WIDTH
#define AC_SDMMC_WIDTH 4
#endif

#ifndef AC_SDMMC_FREQ_KHZ
#define AC_SDMMC_FREQ_KHZ 20000
#endif

#ifndef AC_SDMMC_CLK_GPIO
#define AC_SDMMC_CLK_GPIO -1
#endif

#ifndef AC_SDMMC_CMD_GPIO
#define AC_SDMMC_CMD_GPIO -1
#endif

#ifndef AC_SDMMC_D0_GPIO
#define AC_SDMMC_D0_GPIO -1
#endif

#ifndef AC_SDMMC_D1_GPIO
#define AC_SDMMC_D1_GPIO -1
#endif

#ifndef AC_SDMMC_D2_GPIO
#define AC_SDMMC_D2_GPIO -1
#endif

#ifndef AC_SDMMC_D3_GPIO
#define AC_SDMMC_D3_GPIO -1
#endif

#ifndef AC_SPI_SD_CS_GPIO
#define AC_SPI_SD_CS_GPIO -1
#endif

#ifndef AC_SPI_SD_SCK_GPIO
#define AC_SPI_SD_SCK_GPIO -1
#endif

#ifndef AC_SPI_SD_MISO_GPIO
#define AC_SPI_SD_MISO_GPIO -1
#endif

#ifndef AC_SPI_SD_MOSI_GPIO
#define AC_SPI_SD_MOSI_GPIO -1
#endif

#ifndef AC_SPI_SD_FREQ_HZ
#define AC_SPI_SD_FREQ_HZ 10000000
#endif

static constexpr uint32_t AC_STORAGE_STATUS_POLL_MS = 30000;
static constexpr size_t AC_STORAGE_WRITE_QUEUE_INTERNAL = 4;
static constexpr size_t AC_STORAGE_WRITE_QUEUE_PSRAM = 16;
static constexpr size_t AC_STORAGE_WRITE_CHUNK_BYTES = 512;
static constexpr size_t AC_STORAGE_WRITE_PATH_MAX = 80;
static constexpr size_t AC_STORAGE_WRITE_BUDGET_ITEMS = 2;
static constexpr size_t AC_STORAGE_WRITE_BUDGET_BYTES = 1024;
static constexpr size_t AC_SINK_STREAM_FRAME_BUDGET = 4;
static constexpr uint32_t AC_SINK_ATTACH_RETRY_MS = 2000;

