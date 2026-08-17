/**
 * @file    SoundLevelMeter.h
 * @brief   Измеритель уровня звука (LAeq, A-weighting, FAST/SLOW SPL).
 *
 * ## Семантика уровней
 *
 * Вход — нормированный PCM (−1..1). Все dB-поля: 10·log10(mean² / P0²), P0=1.
 *
 * | Поле Snapshot   | Смысл |
 * |-----------------|-------|
 * | laeq            | LAeq с момента resetLAeq(): mean(A-weighted p²) → dB |
 * | fastSpl         | FAST (τ≈125 мс): экспоненц. RMS A-weighted → dB |
 * | slowSpl         | SLOW (τ≈1 с): экспоненц. RMS A-weighted → dB |
 * | rawRms          | SLOW RMS без A-weighting (линейный mean²) |
 *
 * A-weighting — каскад 4 SOS (IEC 61672-1). Абсолютный SPL в MQTT/WebUI
 * дополняется dspCompensateLevelDb() (DspLevelCompensate.h).
 *
 * ## Snapshot (seqlock)
 *
 * Writer: AudioProducer Core 1 (processFrame). Readers: Core 0 — getSnapshot()
 * без std::mutex: _seq odd=write, even=stable; повтор чтения при нечётном seq.
 *
 * @see DspLevelCompensate.h, docs/ARCHITECTURE.md
 */

#ifndef SOUND_LEVEL_METER_H
#define SOUND_LEVEL_METER_H

#include <Arduino.h>
#include <atomic>
#include <cmath>
#include "Config.h"

class SoundLevelMeter {
public:
    /** Снимок уровней для Core 0 (seqlock, одна копия без mutex). */
    struct Snapshot {
        float laeq;              ///< LAeq [dB] с resetLAeq()
        float fastSpl;           ///< FAST SPL [dB], A-weighted
        float slowSpl;           ///< SLOW SPL [dB], A-weighted
        float rawRms;            ///< SLOW mean² без A-weighting
        uint32_t samplesProcessed; ///< Сэмплов с resetLAeq()
        bool enabled;            ///< processFrame активен
    };

    SoundLevelMeter();

    void reset();
    void processFrame(const int16_t *samples, size_t count);

    float getLAeq() const;
    float getFastSPL() const;
    float getSlowSPL() const;
    float getRawRMS() const;
    uint32_t getSamplesProcessed() const;

    void resetLAeq();
    void setEnabled(bool enabled);
    bool isEnabled() const;
    /** Снимок уровней (seqlock, безопасен с Core 0). */
    Snapshot getSnapshot() const;

    static constexpr int   SAMPLE_RATE   = I2S_SAMPLE_RATE;
    static constexpr int   FRAME_SAMPLES = 512;
    static constexpr float RMS_FAST_SEC  = 0.125f;
    static constexpr float RMS_SLOW_SEC  = 1.000f;
    static constexpr float P0            = 1.0f;

private:
    /** Seqlock: odd = write in progress; even = stable. */
    mutable std::atomic<uint32_t> _seq{0};
    std::atomic<bool> _enabled{true};

    static const float _sosB0[4], _sosB1[4], _sosB2[4];
    static const float _sosA1[4], _sosA2[4];

    float _x1[4], _x2[4], _y1[4], _y2[4];

    float applyAWeighting(float sample);

    float _alphaFast, _alphaSlow;
    float _rms2Fast, _rms2Slow;

    float    _pSumSq;
    uint32_t _sampleCount;

    float _rms2Raw;

    void resetUnlocked();
    void resetLAeqUnlocked();
    void beginWrite();
    void endWrite();
};

#endif
