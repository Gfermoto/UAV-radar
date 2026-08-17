/**
 * @file    HighPassFilter.cpp
 * @brief   Реализация Butterworth HPF 2-го порядка (Direct Form II Transposed).
 *
 * ## Алгоритм
 *
 * Билинейное преобразование аналогового прототипа Butterworth:
 *
 *   1. Нормированная частота:  w0 = 2π * fc / fs
 *   2. Добротность:            alpha = sin(w0) / sqrt(2)
 *   3. Коэффициенты через стандартные формулы HPF
 *   4. Фильтрация: Direct Form II Transposed (4 multiply-accumulate на сэмпл)
 *
 * ## Насыщение
 *
 * Выходное значение обрезается до диапазона int16_t [-32768, 32767]
 * для предотвращения арифметического переполнения.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "HighPassFilter.h"
#include <math.h>

// ── Конструктор ──

HighPassFilter::HighPassFilter(float cutoffHz, float sampleRateHz)
    : _enabled(true)
    , _cutoffHz(cutoffHz)
    , _b0(0), _b1(0), _b2(0)
    , _a1(0), _a2(0)
    , _x1(0), _x2(0)
    , _y1(0), _y2(0)
    , _sampleRate(sampleRateHz)
{
    calculateCoefficients(cutoffHz);
}

// ── Расчёт коэффициентов ──

void HighPassFilter::calculateCoefficients(float cutoffHz) {
    float w0    = 2.0f * M_PI * cutoffHz / _sampleRate;  // Нормированная частота
    float cosW0 = cosf(w0);
    float sinW0 = sinf(w0);
    float alpha = sinW0 / sqrtf(2.0f);                    // Q = 1/√2 (Butterworth)

    float norm  = 1.0f / (1.0f + alpha);                 // Общий знаменатель

    // Стандартные формулы билинейного преобразования для HPF
    _b0 =  (1.0f + cosW0) / 2.0f * norm;
    _b1 = -(1.0f + cosW0)          * norm;
    _b2 = _b0;                                             // Симметричный числитель
    _a1 = -2.0f * cosW0            * norm;
    _a2 =  (1.0f - alpha)          * norm;
}

// ── Посэмпловая обработка ──

int16_t HighPassFilter::processUnlocked(int16_t sample) {
    if (!_enabled) return sample;  // Прозрачный режим

    float x = (float)sample;

    // Direct Form II Transposed: y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    float y = _b0 * x + _b1 * _x1 + _b2 * _x2
            - _a1 * _y1 - _a2 * _y2;

    // Сдвиг состояний
    _x2 = _x1;
    _x1 = x;
    _y2 = _y1;
    _y1 = y;

    // Насыщение до int16_t
    if (y > (float)PCM_MAX_AMP) y = (float)PCM_MAX_AMP;
    if (y < (float)PCM_MIN_AMP) y = (float)PCM_MIN_AMP;

    return (int16_t)y;
}

int16_t HighPassFilter::process(int16_t sample) {
    std::lock_guard<std::mutex> lock(_mutex);
    return processUnlocked(sample);
}

// ── Блочная обработка ──

void HighPassFilter::processBlock(int16_t *buffer, size_t count) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_enabled) return;
    for (size_t i = 0; i < count; i++) {
        buffer[i] = processUnlocked(buffer[i]);
    }
}

// ── Управление ──

void HighPassFilter::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(_mutex);
    _enabled = enabled;
}

bool HighPassFilter::isEnabled() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _enabled;
}

void HighPassFilter::setCutoff(float cutoffHz) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (cutoffHz < 20.0f) cutoffHz = 20.0f;
    if (cutoffHz > _sampleRate * 0.45f) cutoffHz = _sampleRate * 0.45f;
    _cutoffHz = cutoffHz;
    calculateCoefficients(cutoffHz);
    // Сброс состояний — избегаем щелчка при смене fc
    _x1 = _x2 = _y1 = _y2 = 0.0f;
}

float HighPassFilter::getCutoff() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _cutoffHz;
}

HighPassFilter::Snapshot HighPassFilter::getSnapshot() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return {_enabled, _cutoffHz};
}