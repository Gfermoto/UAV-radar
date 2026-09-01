/**
 * @file    SoundLevelMeter.cpp
 * @brief   Реализация шумомера (LAeq, A-weighting, FAST/SLOW) + seqlock snapshot.
 */

#include "SoundLevelMeter.h"

const float SoundLevelMeter::_sosB0[4] = {0.0412367943f, 1.0f, 1.0f, 1.0f};
const float SoundLevelMeter::_sosB1[4] = {0.0824735886f, -2.0f, -2.0f, 0.0f};
const float SoundLevelMeter::_sosB2[4] = {0.0412367943f, 1.0f, 1.0f, 0.0f};
const float SoundLevelMeter::_sosA1[4] = {-1.4449064343f, -1.9154974897f, -1.9838260231f, 0.0f};
const float SoundLevelMeter::_sosA2[4] = {0.5640739647f, 0.9189259102f, 0.9839552843f, 0.0f};

SoundLevelMeter::SoundLevelMeter()
    : _rms2Fast(0.0f), _rms2Slow(0.0f), _rms2Raw(0.0f)
    , _pSumSq(0.0f), _sampleCount(0)
{
    float alphaFast = expf(-1.0f / (static_cast<float>(SAMPLE_RATE) * RMS_FAST_SEC));
    float alphaSlow = expf(-1.0f / (static_cast<float>(SAMPLE_RATE) * RMS_SLOW_SEC));
    _alphaFast = 1.0f - alphaFast;
    _alphaSlow = 1.0f - alphaSlow;
    reset();
}

void SoundLevelMeter::beginWrite() {
    uint32_t s = _seq.load(std::memory_order_relaxed);
    _seq.store(s + 1, std::memory_order_release);  // odd
}

void SoundLevelMeter::endWrite() {
    uint32_t s = _seq.load(std::memory_order_relaxed);
    _seq.store(s + 1, std::memory_order_release);  // even
}

void SoundLevelMeter::reset() {
    beginWrite();
    resetUnlocked();
    endWrite();
}

void SoundLevelMeter::resetUnlocked() {
    for (int s = 0; s < 4; s++) {
        _x1[s] = _x2[s] = _y1[s] = _y2[s] = 0.0f;
    }
    _rms2Fast     = 0.0f;
    _rms2Slow     = 0.0f;
    _rms2Raw      = 0.0f;
    _pSumSq       = 0.0f;
    _sampleCount  = 0;
}

float SoundLevelMeter::applyAWeighting(float x) {
    for (int s = 0; s < 4; s++) {
        if (_sosA1[s] == 0.0f && _sosA2[s] == 0.0f && _sosB1[s] == 0.0f && _sosB2[s] == 0.0f) {
            x = _sosB0[s] * x;
            continue;
        }
        float y = _sosB0[s] * x + _sosB1[s] * _x1[s] + _sosB2[s] * _x2[s]
                - _sosA1[s] * _y1[s] - _sosA2[s] * _y2[s];

        _x2[s] = _x1[s];
        _x1[s] = x;
        _y2[s] = _y1[s];
        _y1[s] = y;
        x = y;
    }
    return x;
}

void SoundLevelMeter::processFrame(const int16_t *samples, size_t count) {
    if (!_enabled.load(std::memory_order_relaxed)) return;

    beginWrite();
    for (size_t i = 0; i < count; i++) {
        float x = static_cast<float>(samples[i]) / 32768.0f;

        _rms2Raw = _alphaSlow * (x * x) + (1.0f - _alphaSlow) * _rms2Raw;

        float p = applyAWeighting(x);

        float p2 = p * p;
        _rms2Fast = _alphaFast * p2 + (1.0f - _alphaFast) * _rms2Fast;
        _rms2Slow = _alphaSlow * p2 + (1.0f - _alphaSlow) * _rms2Slow;

        _pSumSq += p2;
        _sampleCount++;
    }
    endWrite();
}

float SoundLevelMeter::getLAeq() const {
    return getSnapshot().laeq;
}

float SoundLevelMeter::getFastSPL() const {
    return getSnapshot().fastSpl;
}

float SoundLevelMeter::getSlowSPL() const {
    return getSnapshot().slowSpl;
}

float SoundLevelMeter::getRawRMS() const {
    return getSnapshot().rawRms;
}

uint32_t SoundLevelMeter::getSamplesProcessed() const {
    return getSnapshot().samplesProcessed;
}

void SoundLevelMeter::resetLAeq() {
    beginWrite();
    resetLAeqUnlocked();
    endWrite();
}

void SoundLevelMeter::resetLAeqUnlocked() {
    _pSumSq      = 0.0f;
    _sampleCount = 0;
}

void SoundLevelMeter::setEnabled(bool enabled) {
    _enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        beginWrite();
        resetLAeqUnlocked();
        endWrite();
    }
}

bool SoundLevelMeter::isEnabled() const {
    return _enabled.load(std::memory_order_acquire);
}

SoundLevelMeter::Snapshot SoundLevelMeter::getSnapshot() const {
    Snapshot snapshot{};
    for (int spin = 0; spin < 64; ++spin) {
        uint32_t s1 = _seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;

        const float pSumSq = _pSumSq;
        const uint32_t sampleCount = _sampleCount;
        const float rms2Fast = _rms2Fast;
        const float rms2Slow = _rms2Slow;
        const float rms2Raw = _rms2Raw;
        const bool enabled = _enabled.load(std::memory_order_relaxed);

        uint32_t s2 = _seq.load(std::memory_order_acquire);
        if (s1 != s2 || (s2 & 1u)) continue;

        snapshot.laeq = sampleCount == 0
            ? -200.0f
            : 10.0f * log10f(fmaxf(pSumSq / static_cast<float>(sampleCount), 1e-12f) / (P0 * P0));
        snapshot.fastSpl = 10.0f * log10f(fmaxf(rms2Fast, 1e-12f) / (P0 * P0));
        snapshot.slowSpl = 10.0f * log10f(fmaxf(rms2Slow, 1e-12f) / (P0 * P0));
        if (!std::isfinite(snapshot.laeq)) snapshot.laeq = -200.0f;
        if (!std::isfinite(snapshot.fastSpl)) snapshot.fastSpl = -200.0f;
        if (!std::isfinite(snapshot.slowSpl)) snapshot.slowSpl = -200.0f;
        snapshot.rawRms = sqrtf(rms2Raw);
        snapshot.samplesProcessed = sampleCount;
        snapshot.enabled = enabled;
        return snapshot;
    }
    // Spin exhausted — best-effort (редко при конкуренции).
    snapshot.laeq = -200.0f;
    snapshot.fastSpl = -200.0f;
    snapshot.slowSpl = -200.0f;
    snapshot.rawRms = 0.0f;
    snapshot.samplesProcessed = 0;
    snapshot.enabled = _enabled.load(std::memory_order_relaxed);
    return snapshot;
}
