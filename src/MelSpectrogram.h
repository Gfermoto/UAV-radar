/**
 * @file    MelSpectrogram.h
 * @brief   Генератор MEL-спектрограммы на Core 1.
 *
 * ## Назначение
 *
 * Преобразует поток PCM-сэмплов в MEL-спектрограмму — частотно-временное
 * представление для визуализации и анализа звуков природы / птиц.
 *
 * Параметры согласованы с типовыми MEL-банками (16 kHz, 64 bands).
 *
 * Спектрограмма используется для:
 *   - WebUI preview (/api/mel)
 *   - локальной диагностики сигнала
 *
 * ## Алгоритм
 *
 *   1. Накопление 400 сэмплов PCM (25 мс при 16 кГц) + zero-pad до 512
 *   2. Оконная функция Ханна (Hann window, 400 точек)
 *   3. Radix-2 FFT (512 точек, комплексный)
 *   4. Вычисление энергетического спектра
 *   5. Проекция на 64 MEL-фильтра по формуле HTK (треугольные, 125–7500 Гц)
 *   6. Логарифмическая шкала: `log(energy + 1e-10)`
 *   7. ISO 9613-1 (если включено): вычитание α·R [dB]→nat-log из MEL (classify+upload)
 *   8. Сохранение фрейма в кольцевой буфер (401 фрейм ≈ 4 с @ 10 мс hop)
 *
 * ## Параметры
 *
 * | Параметр      | Значение | Обоснование                                    |
 * |---------------|----------|------------------------------------------------|
 * | Window length | 400      | 25 мс, стандарт для AED-моделей (YAMNet, MiniResNetV2) |
 * | FFT size      | 512      | Степень 2, zero-pad 400→512                    |
 * | Hop length    | 160      | 10 мс шаг, ~60% overlap                        |
 * | MEL bands     | 64       | Индустриальный стандарт для публичных моделей  |
 * | Fmin          | 125 Гц   | Отсекает инфразвук, совпадает с HPF=120 Гц     |
 * | Fmax          | 7500 Гц  | Найквист 8 кГц с запасом                       |
 * | MEL formula   | HTK      | Совместимость с YAMNet / MiniResNetV2          |
 * | Frame buffer  | 401      | ~4.01 с @ 10 мс hop (NN patch + archive)       |
 *
 * ## Память
 *
 * Единственный динамический массив в PSRAM: кольцевой буфер фреймов.
 * Окно Ханна и sparse MEL-фильтрбанк — compile-time constant в flash (mel_lut.h).
 *
 * Динамические массивы (PSRAM):
 *   - Буфер фреймов: 401 × 64 × 4 ≈ 100 КБ
 *
 * Статические массивы (flash):
 *   - Окно Ханна: 512 × 4 = 2 КБ (mel_lut.h)
 *   - Sparse filterbank: ~2 КБ (MEL_FB_*; was dense 64.2 КБ)
 *
 * Статические массивы (стек Core 1):
 *   - FFT буфер: 512 × 2 × 4 = 4 КБ
 *   - Спектр мощности: 257 × 4 = 1 КБ
 *
 * ## Производительность
 *
 * На ESP32-S3 @ 240 МГц:
 *   - computeFrame(): ~4 мс (512-pt FFT + фильтрбанк)
 *   - Вызов каждые 10 мс (10 fps)
 *   - Загрузка Core 1: ~4% на MEL-спектрограмму
 *
 * ## Реализации FFT
 *
 * Код автоматически выбирает реализацию:
 *   - Если доступен esp-dsp (IDF-компонент): использует `dsps_fft2r_fc32` (HW-оптимизированный)
 *   - Иначе: чистый C Radix-2 FFT без внешних зависимостей
 *
 * @see mel_lut.h, docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-07
 */

#ifndef MEL_SPECTROGRAM_H
#define MEL_SPECTROGRAM_H

#include <Arduino.h>
#include <atomic>
#include <stdint.h>
#include "Config.h"
#include "mel_lut.h"

static_assert(MEL_WINDOW_LENGTH <= MEL_FFT_SIZE,
              "MEL window must fit in FFT");
static_assert(MEL_LUT_FFT_SIZE == MEL_FFT_SIZE,
              "mel_lut FFT size must match MEL_FFT_SIZE");
static_assert(MEL_LUT_NUM_BANDS == MEL_NUM_BANDS,
              "mel_lut band count must match MEL_NUM_BANDS");
static_assert(MEL_LUT_NUM_BINS == (MEL_FFT_SIZE / 2 + 1),
              "mel_lut bin count must match FFT/2+1");

/**
 * @brief Генератор MEL-спектрограммы.
 *
 * Параметры согласованы с X-CUBE-AI / YAMNet:
 *   64 MEL-полосы, окно 400 (25ms), Fmin=125 Гц, Fmax=7500 Гц.
 *
 * ## Использование
 *
 * @code
 *   MelSpectrogram mel;
 *   mel.begin();
 *   float frame[MelSpectrogram::kNumBands];
 *
 *   // На каждом hop (10 мс):
 *   mel.computeFrame(pcmSamples, frame);  // frame = 64 float
 *   mel.pushFrame(frame);                  // сохранить в буфер
 *
 *   // Для отправки:
 *   const float *spec = mel.getSpectrogram();  // [401][64]
 *   int frames = mel.getFrameCount();
 * @endcode
 *
 * @note computeFrame()/pushFrame() — только Core 1.
 *       Cross-core readers MUST use copyRecentFrames / copyLastPowerSpectrum
 *       (seqlock). getSpectrogram() — single-thread/test only (raw pointer).
 */
class MelSpectrogram {
public:
    MelSpectrogram();
    ~MelSpectrogram();

    /**
     * @brief Инициализация: выделение PSRAM, генерация окна и фильтрбанка.
     * @return true если все структуры успешно созданы.
     */
    bool begin();
    void requestShutdown();
    bool resetForRestart();

    /**
     * @brief Вычисление одного MEL-фрейма из PCM-сэмплов.
     *
     * Выполняет: окно Ханна (400 точек + zero-pad до 512) → FFT → |X|² → MEL-проекция (64 полосы, 125–7500 Гц) → log.
     *
     * @param pcmSamples Указатель на kWindowLength (400) 16-бит сэмплов. Остальные 112 — zero-pad.
     * @param melOut     Выходной массив из kNumBands (64) float.
     */
    void computeFrame(const int16_t *pcmSamples, float *melOut);

    /**
     * @brief Сохранение фрейма в кольцевой буфер спектрограммы.
     * @param melFrame Указатель на kNumBands float.
     */
    void pushFrame(const float *melFrame);

     /**
      * @brief Raw pointer to spectrogram buffer — NOT cross-core safe.
      * Prefer copyRecentFrames(). Kept for single-thread native tests.
      */
     const float* getSpectrogram() const;

    /** @return Количество накопленных фреймов (макс. kNumFrames). */
    int getFrameCount() const;

    /** @return Индекс следующей записи в кольцевом буфере. */
    int getHead() const;

     /**
      * @brief Snapshot последних numFrames фреймов (хронологический порядок).
      *
      * dst layout: [t][band], row-major. Seqlock — без блокировки Core1 writer.
      * @return false если фреймов меньше numFrames или snapshot не стабилен.
      */
     bool copyRecentFrames(float *dst, int numFrames) const;

     /**
      * @brief Thread-safe copy of last FFT power spectrum |X|² [kNumBins].
      * For optional Core‑0 analysis / diagnostics.
      * @return false if no frame computed yet or dst invalid.
      */
     bool copyLastPowerSpectrum(float *dst, int numBins) const;

     bool isReady() const;
     uint32_t getDroppedFrames() const;

     /**
      * Optional ISO 9613-1 band attenuation in dB (power) on log-MEL.
      * Applied inside computeFrame (nat-log units). nullptr/false → off.
      * Safe to call from Core 0 while Core 1 computes frames.
      */
     void setAtmosphericAttenuationDb(const float *attDb64, bool enable);
    bool atmosphericAttenuationEnabled() const {
        return _atmEnabled.load(std::memory_order_acquire);
    }

    // ── Константы из Config.h (доступны для внешнего использования) ──

    static const int kNumBands     = MEL_NUM_BANDS;     ///< 64 MEL-полос
    static const int kWindowLength = MEL_WINDOW_LENGTH;  ///< 400 сэмплов (25 мс)
    static const int kFftSize      = MEL_FFT_SIZE;       ///< 512 точек FFT (zero-pad 400→512)
    static const int kHopLength    = MEL_HOP_LENGTH;     ///< 160 сэмплов (10 мс)
    static const int kNumFrames    = MEL_NUM_FRAMES;     ///< 401 frames (all profiles)
    static const int kNumBins      = MEL_FFT_SIZE / 2 + 1;  ///< 257 частотных бинов

 private:
     /** Ring-buffer seqlock (pushFrame / copyRecentFrames). Odd = write in progress. */
     mutable std::atomic<uint32_t> _bufSeq{0};
     /** Power-spectrum publish seqlock (computeFrame → copyLastPowerSpectrum). */
     mutable std::atomic<uint32_t> _pubSeq{0};
     /**
      * Reentrant writer freeze for large Core0 snapshots.
      * Depth in [1 .. kFreezeSticky) → pushFrame drops.
      * kFreezeSticky and above → tearing down / destroyed (copies abort).
      */
     static constexpr uint32_t kFreezeSticky = 1u << 20;
     mutable std::atomic<uint32_t> _freezeDepth{0};
     std::atomic<bool> _writesEnabled{true};
     std::atomic<bool> _readsEnabled{true};
     std::atomic<uint32_t> _activeWriters{0};
     mutable std::atomic<uint32_t> _activeReaders{0};
     std::atomic<uint32_t> _droppedFrames{0};

     /** Кольцевой буфер фреймов [kNumFrames][kNumBands]. */
     float (*_buffer)[kNumBands];

     int   _head;   ///< Индекс следующей позиции записи
     int   _count;  ///< Количество записанных фреймов

    /// @name Временные массивы (стек Core 1)
    /// @{

    /** Комплексный буфер FFT [kFftSize * 2] (реальная/мнимая части чередуются). */
    float _fftBuf[kFftSize * 2];

    /** Энергетический спектр [kNumBins]. */
    float _powerSpec[kNumBins];

     /** Published snapshot for Core-0 readers (under seqlock). */
     float _powerSpecPub[kNumBins];
     bool  _powerSpecReady = false;

    /** ISO 9613-1 α·R [dB] per MEL band; double-buffer for Core0/Core1. */
    float _atmAttDb[2][kNumBands]{};
    std::atomic<uint8_t> _atmIdx{0};
    std::atomic<bool> _atmEnabled{false};

    /// @}

    // ── MEL-шкала (преобразование Гц ↔ MEL) ──

    /** @brief Перевод частоты в Гц на MEL-шкалу. */
    static float hzToMel(float hz);

    /** @brief Перевод MEL-единиц обратно в Гц. */
    static float melToHz(float mel);
    bool beginWriter();
    void endWriter();
    bool beginReader() const;
    void endReader() const;
    bool waitForQuiescence(uint32_t timeoutMs) const;
};

#endif
