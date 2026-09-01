/**
 * @file    MelSpectrogram.cpp
 * @brief   Реализация генератора MEL-спектрограммы (Radix-2 FFT, Hann, MEL filterbank).
 *
 * ## Алгоритм
 *
 * ### 1. Оконная функция Ханна
 *    hannWindow[i] = 0.5 * (1 - cos(2*pi*i / (N-1)))
 *    Длина: kWindowLength = 400 точек. Окно применяется к первым 400 сэмплам,
 *    остальные 112 — zero-pad для заполнения kFftSize = 512.
 *
 * ### 2. Radix-2 FFT (in-place, complex interleaved)
 *    Стандартный Cooley-Tukey алгоритм с bit-reversal перестановкой.
 *    Вход: [re0, im0, re1, im1, ..., reN-1, imN-1]
 *    На выходе тот же массив содержит частотные компоненты.
 *
 * ### 3. Энергетический спектр
 *    |X[k]|^2 = (re[k]^2 + im[k]^2) / N
 *
 * ### 4. MEL-проекция
 *    64 треугольных фильтра в диапазоне 125–7500 Гц (HTK-формула).
 *    Центр каждого фильтра: melToHz(hzToMel(fmin) + b * melStep)
 *
 * ### 5. Логарифмическая шкала
 *    melOut[b] = log(sum(filterbank[b] * powerSpec) + 1e-10)
 *
 * ## Реализации FFT
 *
 * Код автоматически выбирает:
 *   - esp-dsp (`dsps_fft2r_fc32`) если доступен через IDF-компонент
 *   - чистый C Radix-2 FFT иначе (всегда доступен, без зависимостей)
 *
 * ## Память
 *
 * Все большие массивы в PSRAM через heap_caps_malloc.
 * Временные буферы (_fftBuf, _powerSpec) на стеке Core 1 (5 КБ).
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "MelSpectrogram.h"
#include <string.h>
#include <math.h>
#include <esp_heap_caps.h>

// ── Адаптивный выбор реализации FFT ──

#if __has_include("dsps_fft2r.h")
  #include "dsps_fft2r.h"   // esp-dsp: аппаратно-оптимизированный FFT
  #include "dsps_wind.h"    // esp-dsp: оконные функции
  #include "dsps_mul.h"     // esp-dsp: векторные операции
  #include "dsps_dotprod.h" // esp-dsp: скалярное произведение
  #define MEL_USE_DSP 1
#else
  #define MEL_USE_DSP 0
#endif

// ── Конструктор / Деструктор ──

MelSpectrogram::MelSpectrogram()
    : _buffer(nullptr)
    , _head(0)
    , _count(0)
{
    memset(_fftBuf, 0, sizeof(_fftBuf));
    memset(_powerSpec, 0, sizeof(_powerSpec));
}

MelSpectrogram::~MelSpectrogram() {
    requestShutdown();
    _freezeDepth.fetch_add(kFreezeSticky, std::memory_order_acq_rel);
    // Prefer longer wait over UAF; if still busy, leak buffer (writers may touch it).
    constexpr uint32_t kDtorWaitMs = TASK_STOP_GRACE_MS * 50;  // 5s
    if (!waitForQuiescence(kDtorWaitMs)) {
        _buffer = nullptr;
        return;
    }
    uint32_t seq = _bufSeq.load(std::memory_order_relaxed);
    _bufSeq.store(seq + 1u, std::memory_order_release);
    if (_buffer) heap_caps_free(_buffer);
    _buffer = nullptr;
    _count = 0;
    _head = 0;
    {
        uint32_t pseq = _pubSeq.load(std::memory_order_relaxed);
        if (pseq & 1u) pseq++;
        _pubSeq.store(pseq + 1u, std::memory_order_release);
        _powerSpecReady = false;
        _pubSeq.store(pseq + 2u, std::memory_order_release);
    }
    _bufSeq.store(seq + 2u, std::memory_order_release);
}

// ── MEL-проекция ──

#if !MEL_USE_DSP

/**
 * @brief Комплексный Radix-2 FFT in-place (Cooley-Tukey).
 *
 * Ожидает interleaved формат: [re0, im0, re1, im1, ...].
 * Не использует бит-реверс на выходе (нормальный порядок).
 *
 * @param data Массив комплексных чисел (2*n float).
 * @param n    Количество комплексных точек (должно быть степенью 2).
 */
static void fft(float *data, int n) {
    // ── Шаг 1: Bit-reversal permutation ──
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = data[i*2], ti = data[i*2+1];
            data[i*2] = data[j*2]; data[i*2+1] = data[j*2+1];
            data[j*2] = tr; data[j*2+1] = ti;
        }
    }

    // ── Шаг 2: Поэтапное слияние (Danielson-Lanczos) ──
    for (int len = 2; len <= n; len <<= 1) {
        float wlen = -2.0f * M_PI / len;      // Базовый угол поворота
        float wre = cosf(wlen), wim = sinf(wlen);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;       // Поворотный множитель
            for (int j = 0; j < len/2; j++) {
                int i1 = (i+j)*2, i2 = (i+j+len/2)*2;
                float tr = wr*data[i2] - wi*data[i2+1];
                float ti = wr*data[i2+1] + wi*data[i2];
                data[i2] = data[i1] - tr;  data[i2+1] = data[i1+1] - ti;
                data[i1] += tr;  data[i1+1] += ti;
                // Обновление поворотного множителя
                float nwre = wr*wre - wi*wim;
                wi = wr*wim + wi*wre;
                wr = nwre;
            }
        }
    }
}
#endif

bool MelSpectrogram::beginWriter() {
    if (!_writesEnabled.load(std::memory_order_acquire)) return false;
    _activeWriters.fetch_add(1u, std::memory_order_acq_rel);
    if (_writesEnabled.load(std::memory_order_acquire)) return true;
    _activeWriters.fetch_sub(1u, std::memory_order_acq_rel);
    return false;
}

void MelSpectrogram::endWriter() {
    _activeWriters.fetch_sub(1u, std::memory_order_acq_rel);
}

bool MelSpectrogram::beginReader() const {
    if (!_readsEnabled.load(std::memory_order_acquire)) return false;
    _activeReaders.fetch_add(1u, std::memory_order_acq_rel);
    if (_readsEnabled.load(std::memory_order_acquire)) return true;
    _activeReaders.fetch_sub(1u, std::memory_order_acq_rel);
    return false;
}

void MelSpectrogram::endReader() const {
    _activeReaders.fetch_sub(1u, std::memory_order_acq_rel);
}

bool MelSpectrogram::waitForQuiescence(uint32_t timeoutMs) const {
    const uint32_t started = millis();
    while (_activeWriters.load(std::memory_order_acquire) != 0u ||
           _activeReaders.load(std::memory_order_acquire) != 0u ||
           (_freezeDepth.load(std::memory_order_acquire) != 0u &&
            _freezeDepth.load(std::memory_order_acquire) != kFreezeSticky)) {
        if ((uint32_t)(millis() - started) >= timeoutMs) return false;
#if !defined(UNIT_TEST)
        vTaskDelay(1);
#endif
    }
    return true;
}

// ── Инициализация ──

bool MelSpectrogram::begin() {
    requestShutdown();
    return resetForRestart();
}

void MelSpectrogram::requestShutdown() {
    _readsEnabled.store(false, std::memory_order_release);
    _writesEnabled.store(false, std::memory_order_release);
}

bool MelSpectrogram::resetForRestart() {
    if (_freezeDepth.load(std::memory_order_acquire) >= kFreezeSticky) return false;
    requestShutdown();
    if (!waitForQuiescence(TASK_STOP_GRACE_MS)) return false;
    if (_freezeDepth.load(std::memory_order_acquire) >= kFreezeSticky) return false;

    uint32_t seq = _bufSeq.load(std::memory_order_relaxed);
    _bufSeq.store(seq + 1u, std::memory_order_release);
    if (!_buffer) {
        _buffer = (float(*)[kNumBands])heap_caps_calloc(
            kNumFrames, kNumBands * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_buffer) {
            _bufSeq.store(seq + 2u, std::memory_order_release);
            Serial.printf("[MEL] PSRAM alloc failed\n");
            return false;
        }
    }
    _head = 0;
    _count = 0;
    _droppedFrames.store(0, std::memory_order_relaxed);
    memset(_buffer, 0, kNumFrames * kNumBands * sizeof(float));
    memset(_fftBuf, 0, sizeof(_fftBuf));
    _bufSeq.store(seq + 2u, std::memory_order_release);
    uint32_t pseq = _pubSeq.load(std::memory_order_relaxed);
    _pubSeq.store(pseq + 1u, std::memory_order_release);
    _powerSpecReady = false;
    _pubSeq.store(pseq + 2u, std::memory_order_release);
#if MEL_USE_DSP
    int ret = dsps_fft2r_init_fc32(NULL, kFftSize);
    if (ret != 0) Serial.printf("[MEL] esp-dsp FFT init: %d (using fallback)\n", ret);
#endif
    _readsEnabled.store(true, std::memory_order_release);
    _writesEnabled.store(true, std::memory_order_release);
    return true;
}

// ── Вычисление MEL-фрейма ──

void MelSpectrogram::computeFrame(const int16_t *pcmSamples, float *melOut) {
    if (!pcmSamples || !melOut) return;
    if (!beginWriter()) {
        memset(melOut, 0, (size_t)kNumBands * sizeof(float));
        return;
    }
    // 1. Обнуление FFT-буфера: старый результат может оставить NaN в imag-частях.
    memset(_fftBuf, 0, (size_t)kFftSize * 2 * sizeof(float));
    for (int i = 0; i < kWindowLength; i++) {
        _fftBuf[i * 2]     = (float)pcmSamples[i];
        _fftBuf[i * 2 + 1] = 0.0f;          // Мнимая часть = 0
    }
    // Явный zero-pad: оставшиеся 112 сэмплов обнуляются (убираем остатки FFT)
    for (int i = kWindowLength; i < kFftSize; i++) {
        _fftBuf[i * 2]     = 0.0f;
        _fftBuf[i * 2 + 1] = 0.0f;
    }

#if MEL_USE_DSP
    // Window real samples only (interleaved complex: stride 2).
    dsps_mul_f32(_fftBuf, MEL_HANN_WINDOW, _fftBuf, kWindowLength, 2, 1, 2);
    dsps_fft2r_fc32(_fftBuf, kFftSize);
    dsps_bit_rev_fc32(_fftBuf, kFftSize);
#else
    // Чистый C: умножение на окно (только первые kWindowLength ненулевые)
    for (int i = 0; i < kWindowLength; i++) _fftBuf[i * 2] *= MEL_HANN_WINDOW[i];
    fft(_fftBuf, kFftSize);
#endif

    // 2. Энергетический спектр |X[k]|^2 / N
    for (int i = 0; i < kNumBins; i++) {
        float re = _fftBuf[i * 2];
        float im = _fftBuf[i * 2 + 1];
        _powerSpec[i] = (re * re + im * im) / (float)kFftSize;
    }
    {
        // Separate from ring-buffer seqlock — publish must not invalidate MEL snapshots.
        uint32_t seq = _pubSeq.load(std::memory_order_relaxed);
        if (seq & 1u) seq++;
        _pubSeq.store(seq + 1u, std::memory_order_release);
        memcpy(_powerSpecPub, _powerSpec, sizeof(_powerSpecPub));
        _powerSpecReady = true;
        _pubSeq.store(seq + 2u, std::memory_order_release);
    }

    // 3. MEL-проекция: sparse HTK triangles + log (flash: ~2 KB vs dense 64 KB)
    for (int b = 0; b < kNumBands; b++) {
        float energy = 0.0f;
        const uint16_t n = MEL_FB_LEN[b];
        if (n > 0) {
            const uint16_t start = MEL_FB_START[b];
            const float *w = &MEL_FB_WEIGHTS[MEL_FB_OFFSET[b]];
#if MEL_USE_DSP
            dsps_dotprod_f32(w, &_powerSpec[start], &energy, n);
#else
            for (uint16_t i = 0; i < n; i++) energy += w[i] * _powerSpec[start + i];
#endif
        }
        // Natural log of power — matches WebUI MEL / log-MEL viewers.
        melOut[b] = logf(energy + 1e-10f);
    }

    // Optional ISO 9613-1 power attenuation [dB] → nat-log MEL.
    // ln(E * 10^(-A/10)) = ln(E) - A * ln(10)/10
    if (_atmEnabled.load(std::memory_order_acquire)) {
        constexpr float kDbToNat = 0.230258509f;  // ln(10)/10
        const uint8_t idx = _atmIdx.load(std::memory_order_acquire) & 1u;
        const float *att = _atmAttDb[idx];
        for (int b = 0; b < kNumBands; b++) {
            melOut[b] -= att[b] * kDbToNat;
        }
    }
    endWriter();
}

void MelSpectrogram::setAtmosphericAttenuationDb(const float *attDb64, bool enable) {
    if (!enable || !attDb64) {
        _atmEnabled.store(false, std::memory_order_release);
        return;
    }
    // Write inactive slot, then publish index (Core 1 never sees a torn row).
    const uint8_t cur = _atmIdx.load(std::memory_order_relaxed) & 1u;
    const uint8_t next = cur ^ 1u;
    memcpy(_atmAttDb[next], attDb64, sizeof(_atmAttDb[0]));
    _atmIdx.store(next, std::memory_order_release);
    _atmEnabled.store(true, std::memory_order_release);
}

// ── Кольцевой буфер фреймов ──

void MelSpectrogram::pushFrame(const float *melFrame) {
    if (!melFrame || !beginWriter()) {
        _droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Freeze writers during Core0 snapshots / dtor sticky. Double-check under
    // odd seq so a reader that fetch_add's after the first load cannot race our
    // memcpy (reader waits for even seq before touching _buffer).
    if (_freezeDepth.load(std::memory_order_acquire) > 0u) {
        endWriter();
        _droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t seq = _bufSeq.load(std::memory_order_relaxed);
    if (seq & 1u) seq++;
    _bufSeq.store(seq + 1u, std::memory_order_release);
    if (!_writesEnabled.load(std::memory_order_acquire) || !_buffer ||
        _freezeDepth.load(std::memory_order_acquire) > 0u) {
        _bufSeq.store(seq + 2u, std::memory_order_release);
        endWriter();
        _droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    memcpy(_buffer[_head], melFrame, kNumBands * sizeof(float));
    if (_count < kNumFrames) _count++;
    _head = (_head + 1) % kNumFrames;
    _bufSeq.store(seq + 2u, std::memory_order_release);
    endWriter();
}

bool MelSpectrogram::copyRecentFrames(float *dst, int numFrames) const {
    if (!dst || numFrames <= 0 || numFrames > kNumFrames) return false;
    if (!beginReader()) return false;

    // All snapshots take freeze so begin()/dtor never memset/free during memcpy.
    const uint32_t prev = _freezeDepth.fetch_add(1u, std::memory_order_acq_rel);
    if (prev >= kFreezeSticky) {
        _freezeDepth.fetch_sub(1u, std::memory_order_acq_rel);
        endReader();
        return false;
    }
    for (int wait = 0; wait < 64; ++wait) {
        if ((_bufSeq.load(std::memory_order_acquire) & 1u) == 0u) break;
    }

    bool ok = false;
    const int attempts = (numFrames > 8) ? 2 : 32;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        uint32_t s1 = _bufSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        if (!_buffer) break;
        const int head = _head;
        const int count = _count;
        if (count < numFrames) break;
        for (int t = 0; t < numFrames; t++) {
            int idx = (head - numFrames + t + kNumFrames * 2) % kNumFrames;
            memcpy(dst + t * kNumBands, _buffer[idx],
                   (size_t)kNumBands * sizeof(float));
        }
        uint32_t s2 = _bufSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u)) {
            ok = true;
            break;
        }
    }

    _freezeDepth.fetch_sub(1u, std::memory_order_acq_rel);
    endReader();
    return ok;
}

bool MelSpectrogram::copyLastPowerSpectrum(float *dst, int numBins) const {
    if (!dst || numBins <= 0) return false;
    const int n = (numBins < kNumBins) ? numBins : kNumBins;
    if (!beginReader()) return false;

    bool ok = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = _pubSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        if (!_powerSpecReady) break;
        memcpy(dst, _powerSpecPub, (size_t)n * sizeof(float));
        uint32_t s2 = _pubSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u)) {
            ok = true;
            break;
        }
    }

    endReader();
    return ok;
}

// ── Доступ к спектрограмме ──

const float* MelSpectrogram::getSpectrogram() const { return (const float*)_buffer; }
int MelSpectrogram::getFrameCount() const {
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = _bufSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        int c = _count;
        uint32_t s2 = _bufSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u)) return c;
    }
    return 0;
}
int MelSpectrogram::getHead() const {
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = _bufSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        int h = _head;
        uint32_t s2 = _bufSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u)) return h;
    }
    return 0;
}
bool MelSpectrogram::isReady() const {
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t s1 = _bufSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        bool ok = _buffer != nullptr;
        uint32_t s2 = _bufSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u)) return ok;
    }
    return false;
}
uint32_t MelSpectrogram::getDroppedFrames() const {
    return _droppedFrames.load(std::memory_order_relaxed);
}

// ── MEL-шкала ──

/**
 * @brief Перевод Гц в MEL-единицы.
 *
 * Формула Стивенса-Волкмана (1937):
 *   mel = 2595 * log10(1 + hz / 700)
 */
float MelSpectrogram::hzToMel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

/**
 * @brief Обратное преобразование MEL → Гц.
 *
 *   hz = 700 * (10^(mel/2595) - 1)
 */
float MelSpectrogram::melToHz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}
