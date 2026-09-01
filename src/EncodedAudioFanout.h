/**
 * @file    EncodedAudioFanout.h
 * @brief   Раздача Opus-пакетов нескольким RTSP-consumers (Core 0).
 *
 * Publisher (OpusEncoder) пишет один раз; у каждого consumer своя очередь
 * фиксированной глубины. При переполнении — **drop-oldest** (сохраняем свежесть
 * потока ценой дыр в отстающем клиенте).
 *
 * Потокобезопасность: один `std::mutex` на publish/pop/clear/stats.
 * Не вызывать с Core 1 (аудио realtime).
 *
 * @see docs/ARCHITECTURE.md
 */

#ifndef ENCODED_AUDIO_FANOUT_H
#define ENCODED_AUDIO_FANOUT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

/** Кто читает закодированный поток. */
enum class EncodedAudioConsumer : uint8_t {
    LOCAL_RTSP = 0,   ///< RTSPServer (:554)
    REMOTE_RTSP = 1,  ///< RTSPClient → удалённый сервер
};

struct EncodedAudioPacket {
    static constexpr size_t MAX_BYTES = 1275;  ///< лимит размера Opus-кадра в fanout

    uint8_t data[MAX_BYTES];
    uint16_t size;
    uint32_t timestampMs;  ///< millis() источника на момент encode
};

/** Счётчики очереди одного consumer (для телеметрии/диагностики). */
struct EncodedAudioFanoutStats {
    uint32_t enqueued;
    uint32_t dequeued;
    uint32_t droppedOldest;
    uint32_t droppedOversize;
    uint32_t droppedLockContention;  ///< всегда 0: publish берёт blocking lock
    uint32_t cleared;
    size_t queued;
};

class EncodedAudioFanout {
public:
    static constexpr size_t MAX_QUEUE_DEPTH = 4;

    explicit EncodedAudioFanout(size_t queueDepth = MAX_QUEUE_DEPTH)
        : _depth(queueDepth == 0 ? 1 :
                 (queueDepth > MAX_QUEUE_DEPTH ? MAX_QUEUE_DEPTH : queueDepth)) {}

    /**
     * Опубликовать пакет во все очереди consumers.
     * @return false если data пуст/oversized (считается oversize-drop).
     */
    bool publish(const uint8_t *data, size_t size, uint32_t timestampMs) {
        if (!data || size == 0 || size > EncodedAudioPacket::MAX_BYTES) {
            for (size_t i = 0; i < kConsumerCount; ++i) {
                _oversizeDrops[i].fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        for (size_t i = 0; i < kConsumerCount; ++i) {
            Queue &queue = _queues[i];
            if (queue.count == _depth) {
                queue.head = (queue.head + 1) % _depth;
                queue.count--;
                queue.stats.droppedOldest++;
            }
            const size_t tail = (queue.head + queue.count) % _depth;
            EncodedAudioPacket &packet = queue.packets[tail];
            std::memcpy(packet.data, data, size);
            packet.size = static_cast<uint16_t>(size);
            packet.timestampMs = timestampMs;
            queue.count++;
            queue.stats.enqueued++;
        }
        return true;
    }

    /** Снять один пакет для consumer; false если очередь пуста. */
    bool pop(EncodedAudioConsumer consumer, EncodedAudioPacket &packet) {
        std::lock_guard<std::mutex> lock(_mutex);
        Queue &queue = _queues[indexOf(consumer)];
        if (queue.count == 0) return false;
        packet = queue.packets[queue.head];
        queue.head = (queue.head + 1) % _depth;
        queue.count--;
        queue.stats.dequeued++;
        return true;
    }

    /** Очистить очередь (pause/stop сессии). */
    void clear(EncodedAudioConsumer consumer) {
        std::lock_guard<std::mutex> lock(_mutex);
        Queue &queue = _queues[indexOf(consumer)];
        queue.stats.cleared += static_cast<uint32_t>(queue.count);
        queue.head = 0;
        queue.count = 0;
    }

    EncodedAudioFanoutStats stats(EncodedAudioConsumer consumer) const {
        const size_t index = indexOf(consumer);
        std::lock_guard<std::mutex> lock(_mutex);
        EncodedAudioFanoutStats result = _queues[index].stats;
        result.droppedOversize = _oversizeDrops[index].load(std::memory_order_relaxed);
        result.droppedLockContention = 0;  // publish uses blocking lock
        result.queued = _queues[index].count;
        return result;
    }

private:
    static constexpr size_t kConsumerCount = 2;

    struct Queue {
        EncodedAudioPacket packets[MAX_QUEUE_DEPTH]{};
        size_t head = 0;
        size_t count = 0;
        EncodedAudioFanoutStats stats{};
    };

    static size_t indexOf(EncodedAudioConsumer consumer) {
        return static_cast<size_t>(consumer);
    }

    size_t _depth;
    mutable std::mutex _mutex;
    Queue _queues[kConsumerCount];
    std::atomic<uint32_t> _oversizeDrops[kConsumerCount]{};
};

#endif
