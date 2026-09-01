#pragma once

#include "freertos/FreeRTOS.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

typedef void *RingbufHandle_t;

#ifdef RTSPMIC_STATEFUL_RINGBUF_FAKE

struct FakeRingBuffer {
    explicit FakeRingBuffer(size_t capacityBytes)
        : capacity(capacityBytes) {}
    std::mutex mutex;
    std::deque<std::vector<uint8_t>> chunks;
    size_t capacity;
    size_t used = 0;
};

inline RingbufHandle_t xRingbufferCreate(size_t capacity, int) {
    return new FakeRingBuffer(capacity);
}

inline BaseType_t xRingbufferSend(RingbufHandle_t handle, const void *data,
                                  size_t size, uint32_t) {
    if (!handle || !data || size == 0) return pdFALSE;
    FakeRingBuffer *ring = static_cast<FakeRingBuffer *>(handle);
    std::lock_guard<std::mutex> lock(ring->mutex);
    if (size > ring->capacity - ring->used) return pdFALSE;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    ring->chunks.emplace_back(bytes, bytes + size);
    ring->used += size;
    return pdTRUE;
}

inline void *xRingbufferReceiveUpTo(RingbufHandle_t handle, size_t *itemSize,
                                    uint32_t, size_t maximum) {
    if (itemSize) *itemSize = 0;
    if (!handle || maximum == 0) return nullptr;
    FakeRingBuffer *ring = static_cast<FakeRingBuffer *>(handle);
    std::lock_guard<std::mutex> lock(ring->mutex);
    if (ring->chunks.empty()) return nullptr;

    std::vector<uint8_t> &front = ring->chunks.front();
    const size_t count = std::min(maximum, front.size());
    void *copy = std::malloc(count);
    if (!copy) return nullptr;
    std::memcpy(copy, front.data(), count);
    if (count == front.size()) {
        ring->chunks.pop_front();
    } else {
        front.erase(front.begin(), front.begin() + count);
    }
    ring->used -= count;
    if (itemSize) *itemSize = count;
    return copy;
}

inline void vRingbufferReturnItem(RingbufHandle_t, void *item) {
    std::free(item);
}

inline size_t xRingbufferGetCurFreeSize(RingbufHandle_t handle) {
    if (!handle) return 0;
    FakeRingBuffer *ring = static_cast<FakeRingBuffer *>(handle);
    std::lock_guard<std::mutex> lock(ring->mutex);
    return ring->capacity - ring->used;
}

#else

inline RingbufHandle_t xRingbufferCreate(size_t, int) { return nullptr; }
inline BaseType_t xRingbufferSend(RingbufHandle_t, const void *, size_t, uint32_t) {
    return pdFALSE;
}
inline void *xRingbufferReceiveUpTo(RingbufHandle_t, size_t *, uint32_t, size_t) {
    return nullptr;
}
inline void vRingbufferReturnItem(RingbufHandle_t, void *) {}
inline size_t xRingbufferGetCurFreeSize(RingbufHandle_t) { return 0; }

#endif

#define RINGBUF_TYPE_BYTEBUF 0
