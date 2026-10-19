#include "storage_io_diagnostics.h"

#include <errno.h>
#include <atomic>
#include <stdio.h>

#include "board_storage.h"
#include "debug_log.h"

namespace aircannect::Storage {

namespace {
std::atomic<uint32_t> error_count{0};
std::atomic<bool> first_ready{false};
IoError first_error;

#if AC_STORAGE_SPI_SD_ENABLED
std::atomic<bool> spi_ready{false};
SpiError first_spi;
SpiError pending_spi;
uint32_t spi_started_ms = 0;
uint8_t spi_command = 0xff;
uint32_t spi_argument = 0;
bool spi_active = false;
#endif
}

uint32_t io_error_count() { return error_count.load(std::memory_order_relaxed); }

IoDiagnostics io_diagnostics() {
    IoDiagnostics out;
    out.errors = io_error_count();
    if (first_ready.load(std::memory_order_acquire)) out.first = first_error;
#if AC_STORAGE_SPI_SD_ENABLED
    if (spi_ready.load(std::memory_order_acquire)) out.spi = first_spi;
#endif
    return out;
}

#if AC_STORAGE_SPI_SD_ENABLED
void spi_init_event(uint32_t requested_hz, uint32_t attempt_hz,
                    uint32_t actual_hz, bool selected, const char *reason) {
    const int saved_errno = errno;

    Log::logf_without_file(
        CAT_STORAGE, selected ? LOG_INFO : LOG_WARN,
        "SPI SD init requested_hz=%lu attempt_hz=%lu actual_hz=%lu "
        "result=%s reason=%s\n",
        static_cast<unsigned long>(requested_hz),
        static_cast<unsigned long>(attempt_hz),
        static_cast<unsigned long>(actual_hz), selected ? "selected" : "failed",
        reason);

    errno = saved_errno;
}

void spi_io_begin(uint8_t drive, uint32_t sector, uint32_t count, bool write) {
    spi_active = !spi_ready.load(std::memory_order_relaxed);
    if (!spi_active) return;

    pending_spi = {};
    pending_spi.drive = drive;
    pending_spi.sector = sector;
    pending_spi.count = count;
    pending_spi.write = write;
    spi_command = 0xff;
    spi_argument = 0;
    spi_started_ms = millis();
}

void spi_io_command(uint8_t command, uint32_t argument) {
    if (!spi_active) return;

    spi_command = command;
    spi_argument = argument;
}

void spi_io_fault(const char *phase, int response) {
    if (!spi_active || pending_spi.valid) return;

    const int saved_errno = errno;
    pending_spi.valid = true;
    pending_spi.occurred_ms = millis();
    pending_spi.command = spi_command;
    pending_spi.argument = spi_argument;
    pending_spi.response = response;
    snprintf(pending_spi.phase, sizeof(pending_spi.phase), "%s", phase);
    errno = saved_errno;
}

void spi_io_end(int disk_result) {
    if (!spi_active) return;
    if (disk_result != 0) spi_io_fault("disk_result", disk_result);

    if (pending_spi.valid) {
        pending_spi.disk_result = disk_result;
        pending_spi.elapsed_ms = millis() - spi_started_ms;
        first_spi = pending_spi;
        spi_ready.store(true, std::memory_order_release);
    }
    spi_active = false;
}
#endif

void log_io_error(const char *operation, const char *path, int error,
                  size_t actual, size_t requested) {
    if (error == ENOENT || error == ENOTDIR || error == EEXIST) return;
    ++error_count;

    const int saved_errno = errno;
    static bool reported = false;
    static uint32_t last_report_ms = 0;
    static uint32_t suppressed = 0;
    const uint32_t now = millis();

    if (!first_ready.load(std::memory_order_relaxed)) {
        first_error.valid = true;
        first_error.occurred_ms = now;
        first_error.error = error;
        first_error.actual = actual;
        first_error.requested = requested;
        snprintf(first_error.operation, sizeof(first_error.operation), "%s",
                 operation);
        snprintf(first_error.path, sizeof(first_error.path), "%s", path ? path : "--");
        first_ready.store(true, std::memory_order_release);
    }

    if (reported && static_cast<uint32_t>(now - last_report_ms) < 1000) {
        ++suppressed;
        errno = saved_errno;
        return;
    }

    Log::logf_without_file(CAT_STORAGE, LOG_ERROR,
              "I/O op=%s errno=%d bytes=%u/%u suppressed=%lu path=%s\n",
              operation, error, static_cast<unsigned>(actual),
              static_cast<unsigned>(requested),
              static_cast<unsigned long>(suppressed), path ? path : "--");

    reported = true;
    last_report_ms = now;
    suppressed = 0;
    errno = saved_errno;
}

}  // namespace aircannect::Storage
