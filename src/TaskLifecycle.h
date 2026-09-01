/**
 * @file    TaskLifecycle.h
 * @brief   Атомарный жизненный цикл FreeRTOS-задачи (без семафоров).
 *
 * Состояния: STOPPED → STARTING → RUNNING | START_FAILED; STOP_REQUESTED → EXITED.
 * prepareStart() CAS; publishHandle/markRunning/markExited — handshake с FreeRtosTaskHandshake.
 *
 * @see FreeRtosTaskHandshake.h, docs/ARCHITECTURE.md
 */

#ifndef TASK_LIFECYCLE_H
#define TASK_LIFECYCLE_H

#include <atomic>

/** Состояние задачи (atomic, memory_order acquire/release). */
enum class TaskLifecycleState {
    STOPPED,
    STARTING,
    RUNNING,
    STOP_REQUESTED,
    START_FAILED,
    EXITED,
};

class TaskLifecycle {
public:
    bool prepareStart() {
        TaskLifecycleState current = _state.load(std::memory_order_acquire);
        while (current == TaskLifecycleState::STOPPED ||
               current == TaskLifecycleState::EXITED) {
            if (_state.compare_exchange_weak(
                    current, TaskLifecycleState::STARTING,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                _handle.store(nullptr, std::memory_order_release);
                _startFailed.store(false, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    void publishHandle(void *handle) {
        _handle.store(handle, std::memory_order_release);
    }

    void markRunning() {
        TaskLifecycleState expected = TaskLifecycleState::STARTING;
        _state.compare_exchange_strong(
            expected, TaskLifecycleState::RUNNING,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void markStartFailed() {
        _startFailed.store(true, std::memory_order_release);
        _state.store(TaskLifecycleState::START_FAILED, std::memory_order_release);
    }

    void requestStop() {
        TaskLifecycleState current = _state.load(std::memory_order_acquire);
        if (current == TaskLifecycleState::RUNNING ||
            current == TaskLifecycleState::STARTING) {
            _state.store(TaskLifecycleState::STOP_REQUESTED, std::memory_order_release);
        }
    }

    bool stopRequested() const {
        return _state.load(std::memory_order_acquire) ==
               TaskLifecycleState::STOP_REQUESTED;
    }

    void markExited() {
        _handle.store(nullptr, std::memory_order_release);
        _state.store(TaskLifecycleState::EXITED, std::memory_order_release);
    }

    TaskLifecycleState state() const {
        return _state.load(std::memory_order_acquire);
    }

    void *handle() const {
        return _handle.load(std::memory_order_acquire);
    }

    bool startFailed() const {
        return _startFailed.load(std::memory_order_acquire);
    }

    bool canReleaseResources() const {
        TaskLifecycleState current = state();
        return current == TaskLifecycleState::STOPPED ||
               current == TaskLifecycleState::EXITED;
    }

private:
    std::atomic<TaskLifecycleState> _state{TaskLifecycleState::STOPPED};
    std::atomic<void *> _handle{nullptr};
    std::atomic<bool> _startFailed{false};
};

#endif
