#pragma once

#include <stdint.h>
#include <stddef.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

typedef int i2s_port_t;
typedef int i2s_mode_t;
typedef int esp_err_t;

#define I2S_NUM_0 0
#define I2S_MODE_MASTER 0x01
#define I2S_MODE_RX 0x02
#define I2S_CHANNEL_STEREO 0
#define I2S_BITS_PER_SAMPLE_32BIT 0
#define I2S_BITS_PER_CHANNEL_32BIT 0
#define I2S_CHANNEL_FMT_RIGHT_LEFT 0
#define I2S_COMM_FORMAT_STAND_I2S 0
#define I2S_PIN_NO_CHANGE (-1)
#define ESP_INTR_FLAG_LEVEL2 2
#define ESP_OK 0

typedef struct {
    int mode;
    int sample_rate;
    int bits_per_sample;
    int channel_format;
    int communication_format;
    int intr_alloc_flags;
    int dma_buf_count;
    int dma_buf_len;
    int use_apll;
    int tx_desc_auto_clear;
    int fixed_mclk;
} i2s_config_t;

typedef struct {
    int bck_io_num;
    int ws_io_num;
    int data_out_num;
    int data_in_num;
} i2s_pin_config_t;

struct FakeI2sState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<int32_t> samples;
    bool installed = false;
};

inline FakeI2sState g_fakeI2s;

inline int i2s_driver_install(i2s_port_t, const i2s_config_t *, int, void *) {
    std::lock_guard<std::mutex> lock(g_fakeI2s.mutex);
    g_fakeI2s.installed = true;
    return ESP_OK;
}
inline int i2s_set_pin(i2s_port_t, const i2s_pin_config_t *) { return 0; }
inline int i2s_zero_dma_buffer(i2s_port_t) { return 0; }
inline int i2s_read(i2s_port_t, void *destination, size_t bytesRequested,
                    size_t *bytesRead, uint32_t ticks) {
    if (bytesRead) *bytesRead = 0;
#ifdef RTSPMIC_STATEFUL_I2S_FAKE
    std::unique_lock<std::mutex> lock(g_fakeI2s.mutex);
    g_fakeI2s.cv.wait_for(lock, std::chrono::milliseconds(ticks), []() {
        return !g_fakeI2s.samples.empty() || !g_fakeI2s.installed;
    });
    if (!g_fakeI2s.installed || g_fakeI2s.samples.empty()) return ESP_OK;
    const size_t count =
        std::min(bytesRequested / sizeof(int32_t), g_fakeI2s.samples.size());
    int32_t *output = static_cast<int32_t *>(destination);
    for (size_t i = 0; i < count; ++i) {
        output[i] = g_fakeI2s.samples.front();
        g_fakeI2s.samples.pop_front();
    }
    if (bytesRead) *bytesRead = count * sizeof(int32_t);
#else
    (void)destination;
    (void)bytesRequested;
    (void)ticks;
#endif
    return ESP_OK;
}
inline int i2s_driver_uninstall(i2s_port_t) {
    {
        std::lock_guard<std::mutex> lock(g_fakeI2s.mutex);
        g_fakeI2s.installed = false;
    }
    g_fakeI2s.cv.notify_all();
    return ESP_OK;
}

inline void test_i2s_reset() {
    std::lock_guard<std::mutex> lock(g_fakeI2s.mutex);
    g_fakeI2s.samples.clear();
    g_fakeI2s.installed = false;
}

inline void test_i2s_push(const int32_t *samples, size_t count) {
    {
        std::lock_guard<std::mutex> lock(g_fakeI2s.mutex);
        g_fakeI2s.samples.insert(g_fakeI2s.samples.end(), samples, samples + count);
    }
    g_fakeI2s.cv.notify_one();
}
