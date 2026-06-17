/* ESP32 Arduino compatibility wrapper for libsmb2 */
#ifndef ESP_COMPAT_WRAPPER_H
#define ESP_COMPAT_WRAPPER_H

/* Include standard headers needed for libsmb2 on ESP32 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <esp_heap_caps.h>

/* Define random functions for ESP32 */
#define smb2_random esp_random
#define smb2_srandom(seed) /* ESP32 RNG doesn't need seeding */

/* Define login_num for getlogin_r */
#define login_num ENXIO

/* Declare esp_random if not already declared */
#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);

static inline void *smb_psram_alloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static inline void *smb_psram_calloc(size_t count, size_t size) {
    void *ptr = heap_caps_calloc(count,
                                 size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_calloc(count,
                               size,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static inline void *smb_psram_realloc(void *ptr, size_t size) {
    if (size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    void *out = heap_caps_realloc(ptr,
                                  size,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        out = heap_caps_realloc(ptr,
                                size,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return out;
}

#define malloc smb_psram_alloc
#define calloc smb_psram_calloc
#define realloc smb_psram_realloc
#define free heap_caps_free

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPAT_WRAPPER_H */
