/**
 * @file    AudioLifecycle.h
 * @brief   Согласованный pause/stop/start источника PCM и RTSP-consumers.
 *
 * Порядок stop: сначала pause consumers (чтобы не читали пустой fanout),
 * затем stop источника (AudioProducer). Start — наоборот.
 *
 * - `restart()` — try_lock: при contention → BUSY (не блокировать caller).
 * - `pauseAndStop` / `startAndResume` — **blocking** mutex: thermal shutdown
 *   не должен терять stop из‑за BUSY.
 *
 * @see docs/ARCHITECTURE.md, SystemMonitor (thermal)
 */

#ifndef AUDIO_LIFECYCLE_H
#define AUDIO_LIFECYCLE_H

#include <cstddef>
#include <mutex>

/** Источник PCM (обычно AudioProducer). */
class AudioLifecycleSource {
public:
    virtual ~AudioLifecycleSource() = default;
    virtual void stopAudio() = 0;
    virtual bool startAudio() = 0;
};

/** Consumer закодированного/сессионного аудио (RTSP server/client). */
class AudioLifecycleConsumer {
public:
    virtual ~AudioLifecycleConsumer() = default;
    virtual void pauseAudio() = 0;
    virtual void resumeAudio() = 0;
};

enum class AudioLifecycleResult {
    RESTARTED,
    STOPPED,
    STARTED,
    START_FAILED,
    BUSY,  ///< restart() не взял mutex
};

class AudioLifecycleCoordinator {
public:
    static constexpr size_t MAX_CONSUMERS = 4;

    AudioLifecycleCoordinator(AudioLifecycleSource &source,
                              AudioLifecycleConsumer *const *consumers,
                              size_t consumerCount)
        : _source(source),
          _consumerCount(consumerCount > MAX_CONSUMERS ? MAX_CONSUMERS : consumerCount) {
        for (size_t i = 0; i < _consumerCount; ++i) _consumers[i] = consumers[i];
    }

    /** Pause → stop → start → resume. try_lock; при занятости — BUSY. */
    AudioLifecycleResult restart() {
        std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return AudioLifecycleResult::BUSY;
        pauseConsumers();
        _source.stopAudio();
        if (!_source.startAudio()) return AudioLifecycleResult::START_FAILED;
        resumeConsumers();
        return AudioLifecycleResult::RESTARTED;
    }

    /** Блокирующий stop (thermal): не терять остановку на try_lock. */
    AudioLifecycleResult pauseAndStop() {
        std::lock_guard<std::mutex> lock(_mutex);
        pauseConsumers();
        _source.stopAudio();
        return AudioLifecycleResult::STOPPED;
    }

    /** Блокирующий start; пара к pauseAndStop. */
    AudioLifecycleResult startAndResume() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_source.startAudio()) return AudioLifecycleResult::START_FAILED;
        resumeConsumers();
        return AudioLifecycleResult::STARTED;
    }

    bool isPaused() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _paused;
    }

private:
    void pauseConsumers() {
        if (_paused) return;
        for (size_t i = 0; i < _consumerCount; ++i) {
            if (_consumers[i]) _consumers[i]->pauseAudio();
        }
        _paused = true;
    }

    void resumeConsumers() {
        if (!_paused) return;
        for (size_t i = 0; i < _consumerCount; ++i) {
            if (_consumers[i]) _consumers[i]->resumeAudio();
        }
        _paused = false;
    }

    AudioLifecycleSource &_source;
    AudioLifecycleConsumer *_consumers[MAX_CONSUMERS]{};
    size_t _consumerCount;
    mutable std::mutex _mutex;
    bool _paused = false;
};

#endif
