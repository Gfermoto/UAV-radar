#pragma once

#include <stdint.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <thread>

#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY     0xFFFFFFFFu
#define pdTRUE            1
#define pdFALSE           0
#define pdPASS            1
#define pdFAIL            0

typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *RingbufHandle_t;

struct FakeSemaphore {
    FakeSemaphore(unsigned initial, bool isMutex)
        : count(initial), mutexKind(isMutex) {}
    std::mutex mutex;
    std::condition_variable cv;
    unsigned count;
    bool mutexKind;
};

inline thread_local unsigned g_fakeHeldMutexes = 0;

inline bool fake_freertos_mutex_held_by_current_thread() {
    return g_fakeHeldMutexes != 0;
}

inline void vTaskDelay(uint32_t ticks) {
#ifdef RTSPMIC_STATEFUL_TASK_FAKE
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
#else
    (void)ticks;
#endif
}
inline void vTaskDelete(void *) {}
inline uint32_t uxTaskGetStackHighWaterMark(void *) { return 0; }
inline uint32_t uxTaskGetNumberOfTasks() { return 0; }
inline void taskYIELD() { std::this_thread::yield(); }

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    return new FakeSemaphore(1, true);
}
inline SemaphoreHandle_t xSemaphoreCreateBinary() {
    return new FakeSemaphore(0, false);
}
inline int xSemaphoreTake(SemaphoreHandle_t handle, uint32_t ticks) {
    if (!handle) return pdFALSE;
    FakeSemaphore *sem = static_cast<FakeSemaphore *>(handle);
    std::unique_lock<std::mutex> lock(sem->mutex);
    if (ticks == 0) {
        if (sem->count == 0) return pdFALSE;
    } else if (ticks == portMAX_DELAY) {
        sem->cv.wait(lock, [&]() { return sem->count > 0; });
    } else if (!sem->cv.wait_for(
                   lock, std::chrono::milliseconds(ticks),
                   [&]() { return sem->count > 0; })) {
        return pdFALSE;
    }
    --sem->count;
    if (sem->mutexKind) ++g_fakeHeldMutexes;
    return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t handle) {
    if (!handle) return;
    FakeSemaphore *sem = static_cast<FakeSemaphore *>(handle);
    if (sem->mutexKind && g_fakeHeldMutexes > 0) --g_fakeHeldMutexes;
    {
        std::lock_guard<std::mutex> lock(sem->mutex);
        sem->count = 1;
    }
    sem->cv.notify_one();
}
inline void vSemaphoreDelete(SemaphoreHandle_t handle) {
    delete static_cast<FakeSemaphore *>(handle);
}

inline int xTaskCreatePinnedToCore(void (*entry)(void *), const char *, uint32_t,
                                   void *argument, uint32_t, TaskHandle_t *handle,
                                   int) {
#ifdef RTSPMIC_STATEFUL_TASK_FAKE
    static std::atomic<uintptr_t> nextHandle{1};
    if (!entry || !handle) return pdFAIL;
    *handle = reinterpret_cast<TaskHandle_t>(
        nextHandle.fetch_add(1, std::memory_order_relaxed));
    std::thread([entry, argument]() { entry(argument); }).detach();
    return pdPASS;
#else
    (void)entry;
    (void)argument;
    (void)handle;
    return 0;
#endif
}
