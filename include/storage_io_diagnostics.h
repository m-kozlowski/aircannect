#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_storage.h"

namespace aircannect::Storage {

struct IoError {
    bool valid = false;
    uint32_t occurred_ms = 0;
    int error = 0;
    size_t actual = 0;
    size_t requested = 0;
    char operation[16] = {};
    char path[256] = {};
};

struct SpiError {
    bool valid = false;
    bool write = false;
    uint8_t drive = 0;
    uint8_t command = 0xff;
    uint32_t argument = 0;
    uint32_t sector = 0;
    uint32_t count = 0;
    uint32_t occurred_ms = 0;
    uint32_t elapsed_ms = 0;
    int response = -1;
    int disk_result = 0;
    char phase[16] = {};
};

struct IoDiagnostics {
    uint32_t errors = 0;
    IoError first;
    SpiError spi;
};

// Immutable first-error records, published by the storage owner until reboot.
// Readers never wait for the worker or perform filesystem operations.
IoDiagnostics io_diagnostics();

// Storage-task-only. Expected missing paths and ordinary EOF are not I/O errors.
void log_io_error(const char *operation, const char *path, int error,
                  size_t actual = 0, size_t requested = 0);
uint32_t io_error_count();

// Arduino SD diskio hooks.
#if AC_STORAGE_SPI_SD_ENABLED
// Mount-owner-only events, emitted with the SPI transaction released.
// Storage publishes the selected clock after mounting, never from this hook.
void spi_init_event(uint32_t requested_hz, uint32_t attempt_hz,
                    uint32_t actual_hz, bool selected, const char *reason);

// Transfer instrumentation. No logging or allocation on the SPI bus.
void spi_io_begin(uint8_t drive, uint32_t sector, uint32_t count, bool write);
void spi_io_command(uint8_t command, uint32_t argument);
void spi_io_fault(const char *phase, int response);
void spi_io_end(int disk_result);
#else
inline void spi_init_event(uint32_t, uint32_t, uint32_t, bool, const char *) {}

inline void spi_io_begin(uint8_t, uint32_t, uint32_t, bool) {}
inline void spi_io_command(uint8_t, uint32_t) {}
inline void spi_io_fault(const char *, int) {}
inline void spi_io_end(int) {}
#endif

}  // namespace aircannect::Storage
