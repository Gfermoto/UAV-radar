/**
 * @file    PcmFrameAccumulator.h
 * @brief   Кольцевой накопитель PCM и аудио-часы для фиксированных фреймов.
 *
 * PcmFrameAccumulator — append/popFrame для Opus (320 сэмплов) и др.
 * AudioFrameClock — монотонный timestamp_ms из накопленных сэмплов.
 *
 * @see OpusEncoder.h, docs/ARCHITECTURE.md
 */

#ifndef PCM_FRAME_ACCUMULATOR_H
#define PCM_FRAME_ACCUMULATOR_H

#include <cstddef>
#include <cstdint>

/** Накопитель PCM: append до CapacitySamples, popFrame при FrameSamples. */
template <size_t FrameSamples, size_t CapacitySamples>
class PcmFrameAccumulator {
    static_assert(FrameSamples > 0, "frame size must be positive");
    static_assert(CapacitySamples >= FrameSamples, "capacity must fit one frame");

public:
    bool append(const int16_t *samples, size_t count) {
        if ((!samples && count != 0) || count > CapacitySamples - _count) return false;
        for (size_t i = 0; i < count; ++i) {
            _samples[(_head + _count + i) % CapacitySamples] = samples[i];
        }
        _count += count;
        return true;
    }

    bool popFrame(int16_t *frame) {
        if (!frame || _count < FrameSamples) return false;
        for (size_t i = 0; i < FrameSamples; ++i) {
            frame[i] = _samples[(_head + i) % CapacitySamples];
        }
        _head = (_head + FrameSamples) % CapacitySamples;
        _count -= FrameSamples;
        return true;
    }

    size_t pending() const { return _count; }
    size_t freeSpace() const { return CapacitySamples - _count; }
    void clear() { _head = 0; _count = 0; }

private:
    int16_t _samples[CapacitySamples]{};
    size_t _head = 0;
    size_t _count = 0;
};

/** Аудио-часы: timestamp_ms = samplesConsumed * 1000 / sampleRate. */
class AudioFrameClock {
public:
    explicit AudioFrameClock(uint32_t sampleRate) : _sampleRate(sampleRate) {}

    uint32_t consumeFrame(size_t sampleCount) {
        const uint32_t timestamp =
            _sampleRate == 0 ? 0 :
            static_cast<uint32_t>((_samplesConsumed * 1000ULL) / _sampleRate);
        _samplesConsumed += sampleCount;
        return timestamp;
    }

    void reset() { _samplesConsumed = 0; }

private:
    uint32_t _sampleRate;
    uint64_t _samplesConsumed = 0;
};

#endif
