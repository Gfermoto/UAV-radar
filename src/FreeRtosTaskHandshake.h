/**
 * @file    FreeRtosTaskHandshake.h
 * @brief   Семафорный handshake старта/остановки FreeRTOS-задачи + TaskLifecycle.
 *
 * prepareStart → releaseTask → waitForRelease (в задаче) → signalStartup → waitStartup.
 * requestStop → stopRequested → signalExit → waitExit. Бинарные семафоры startGate/startupAck/exitAck.
 *
 * @see TaskLifecycle.h, OpusEncoder.h, docs/ARCHITECTURE.md
 */

#ifndef FREE_RTOS_TASK_HANDSHAKE_H
#define FREE_RTOS_TASK_HANDSHAKE_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "TaskLifecycle.h"

class FreeRtosTaskHandshake {
public:
    bool prepareStart() {
        if (!ensureSemaphores() || !_lifecycle.prepareStart()) return false;
        xSemaphoreTake(_startGate, 0);
        xSemaphoreTake(_startupAck, 0);
        xSemaphoreTake(_exitAck, 0);
        return true;
    }

    void publishHandle(TaskHandle_t handle) {
        _lifecycle.publishHandle(static_cast<void *>(handle));
    }

    TaskHandle_t handle() const {
        return static_cast<TaskHandle_t>(_lifecycle.handle());
    }

    void releaseTask() { xSemaphoreGive(_startGate); }
    void waitForRelease() { xSemaphoreTake(_startGate, portMAX_DELAY); }

    void signalStartup(bool success) {
        if (success) _lifecycle.markRunning();
        else _lifecycle.markStartFailed();
        xSemaphoreGive(_startupAck);
    }

    bool waitStartup(uint32_t ticks) {
        return xSemaphoreTake(_startupAck, ticks) == pdTRUE &&
               !_lifecycle.startFailed();
    }

    void requestStop() { _lifecycle.requestStop(); }
    bool stopRequested() const { return _lifecycle.stopRequested(); }
    void abortStart() { _lifecycle.markExited(); }

    void signalExit() {
        _lifecycle.markExited();
        xSemaphoreGive(_exitAck);
    }

    bool waitExit(uint32_t ticks) {
        return xSemaphoreTake(_exitAck, ticks) == pdTRUE;
    }

    bool startFailed() const { return _lifecycle.startFailed(); }
    bool canReleaseResources() const { return _lifecycle.canReleaseResources(); }

private:
    bool ensureSemaphores() {
        if (_startGate && _startupAck && _exitAck) return true;
        _startGate = xSemaphoreCreateBinary();
        _startupAck = xSemaphoreCreateBinary();
        _exitAck = xSemaphoreCreateBinary();
        if (_startGate && _startupAck && _exitAck) return true;
        if (_startGate) vSemaphoreDelete(_startGate);
        if (_startupAck) vSemaphoreDelete(_startupAck);
        if (_exitAck) vSemaphoreDelete(_exitAck);
        _startGate = nullptr;
        _startupAck = nullptr;
        _exitAck = nullptr;
        return false;
    }

    TaskLifecycle _lifecycle;
    SemaphoreHandle_t _startGate = nullptr;
    SemaphoreHandle_t _startupAck = nullptr;
    SemaphoreHandle_t _exitAck = nullptr;
};

#endif
