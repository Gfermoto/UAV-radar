/**
 * @file    esp_heap_caps.h
 * @brief   Native stub для PSRAM-аллокатора (тесты используют обычный malloc).
 */

#pragma once

#include <cstdlib>
#include <cstdint>

#define MALLOC_CAP_SPIRAM 0x400
#define MALLOC_CAP_8BIT   0x200
#define MALLOC_CAP_DMA    0x100
#define MALLOC_CAP_INTERNAL 0x080

inline uint32_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 0x100000;  /* ~1 MB — достаточно для любых тестов */
}

inline uint32_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return 0x80000;  /* ~512 KB contiguous */
}

inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return std::malloc(size);
}

inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return std::calloc(n, size);
}

inline void heap_caps_free(void *ptr) {
    std::free(ptr);
}
