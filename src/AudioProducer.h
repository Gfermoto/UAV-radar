/**
 * @file    AudioProducer.h
 * @brief   Захват аудио с XVF3800 через I2S DMA, Lock-Free Ring Buffer. Core 1.
 *
 * ## Назначение
 *
 * AudioProducer — критический модуль реального времени. Захватывает
 * аудиопоток с XVF3800 через I2S (32-бит стерео), конвертирует в 16-бит
 * моно PCM и отправляет в кольцевой буфер для потребителей на Core 0.
 *
 * Выполняется строго на Core 1 с максимальным приоритетом (10), чтобы
 * избежать пропуска DMA-буферов. Core 1 НЕ выполняет сетевых операций.
 *
 * ## Пайплайн
 *
 * ```
 * I2S DMA (32-bit stereo, 16 кГц, аппаратный)
 *   → convertI2SToMono (32→16 бит, стерео→моно, усиление)
 *   → xRingbufferSend (lock-free byte buffer, 64 КБ = 2 сек)
 *   → AudioTelemetry (RMS, peak, clipping, dB)
 *   → MelSpectrogram (40-band MEL, 10 ms hop)
 *
 * ФВЧ: только аппаратный XVF AEC_HPFONOFF (не софт-Butterworth).
 * ```
 *
 * ## Потребители (Core 0)
 *
 * - **RTSP Client** — отправка аудио на удалённый сервер
 * - **RTSP Server** — раздача аудио локальным клиентам
 * - **OpusEncoder** — сжатие в Opus для передачи
 * - **MQTT Manager** — публикация телеметрии
 *
 * ## Потокобезопасность
 *
 * - xRingbuffer — lock-free, single-producer (Core 1), single-consumer (Core 0)
 * - _telemetryMutex — защита телеметрии и параметров усиления
 * - readSamples() — потокобезопасное чтение из Ring Buffer
 * - Mel copyRecentFrames() — seqlock snapshot с любого ядра
 *
 * ## Архитектурные решения
 *
 * - DMA-буфер выделен во ВНУТРЕННЕЙ SRAM (MALLOC_CAP_DMA), не в PSRAM
 *   (PSRAM непредсказуема по задержке для DMA-транзакций)
 * - xRingbuffer вместо StreamBuffer + Mutex (устранение priority inversion)
 * - lastSampleTimeMs для Audio Liveness Watchdog (SystemMonitor)
 *
 * ## Источники
 *
 * - oboe.com — паттерны работы с аудио в реальном времени
 * - ESP32-A2DP BluetoothA2DPSinkQueued — lock-free рингбуфер
 * - Sukecz/esp32-birdnet-mic — I2S захват для ML-инференса
 *
 * @see docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef AUDIO_PRODUCER_H
#define AUDIO_PRODUCER_H

#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/ringbuf.h>
#include <atomic>
#include "Config.h"
#include "AudioTelemetry.h"
#include "MelSpectrogram.h"
#include "SoundLevelMeter.h"
#include "AudioLifecycle.h"
#include "FreeRtosTaskHandshake.h"
#include "HighPassFilter.h"

class XVF3800_I2C;

/**
 * @brief Производитель аудио — I2S захват, HPF, MEL, Ring Buffer. Core 1.
 *
 * ## Использование
 *
 * @code
 *   AudioProducer audio;
 *   audio.begin();  // запуск задачи на Core 1
 *
 *   // На Core 0 (чтение из Ring Buffer):
 *   int16_t buf[512];
 *   size_t samples = audio.readSamples(buf, 512);
 *
 *   // Телеметрия:
 *   AudioTelemetry t;
 *   audio.getTelemetry(t);  // t.rms, t.peak, t.clipping, t.avg_db
 *
 *   // Спектрограмма:
 *   const MelSpectrogram *mel = audio.getMelSpectrogram();
 * @endcode
 */
class AudioProducer : public AudioLifecycleSource {
public:
    AudioProducer();

    /**
     * @brief Запуск I2S, Ring Buffer, задачи на Core 1.
     * @return true если всё инициализировано успешно.
     */
    bool begin();

    /** @brief Остановка задачи и освобождение I2S/RingBuffer. */
    void stop();
    void stopAudio() override { stop(); }
    bool startAudio() override { return begin(); }

    /**
     * @brief Чтение PCM-сэмплов из кольцевого буфера (Core 0).
     *
     * Lock-free. Блокируется до 10 мс при отсутствии данных.
     *
     * @param buffer     Выходной буфер для сэмплов.
     * @param maxSamples Максимальное количество сэмплов.
     * @return Фактическое количество прочитанных сэмплов (0 если нет данных).
     */
    size_t readSamples(int16_t *buffer, size_t maxSamples);

    /**
     * @brief Количество доступных сэмплов в кольцевом буфере.
     * @return Количество сэмплов (не байт).
     */
    size_t available() const;

    /** @brief Получение телеметрии (потокобезопасно). */
    void getTelemetry(AudioTelemetry &telemetry);

    /** @brief Установка программного усиления (множитель). */
    void setGain(float gain);

    /** @return Текущее усиление. */
    float getGain();

    /** Указатель на XVF; HPF смотрит isInitialized(), не только non-null. */
    void setXvfPointer(XVF3800_I2C *xvf);
    XVF3800_I2C *getXvf() const { return _xvf; }
    /**
     * @brief AEC_HPFONOFF mode 0..4 (Off/70/125/150/180 Гц).
     * Живой XVF (isInitialized): hardware HPF only.
     * Нет чипа / begin failed: soft Butterworth (без double-filter).
     */
    void setHpfMode(uint8_t mode);
    /** @brief Legacy: on→mode 2 (125 Гц), off→0. */
    void setHpfEnabled(bool enabled);

    /** @return true если HPF активен (mode != 0). */
    bool isHpfEnabled() const;
    /** @return текущий mode 0..4. */
    uint8_t getHpfMode() const;

    /** @brief Частота среза HPF (Гц) по таблице XMOS; 0 если Off. */
    float getHpfCutoff() const;

    /** @brief Доступ к MEL-спектрограмме (только для чтения). */
    const MelSpectrogram* getMelSpectrogram() const { return &_mel; }
    MelSpectrogram* getMelSpectrogram() { return &_mel; }

    /** Forward ISO 9613-1 MEL attenuation into MelSpectrogram. */
    void setMelAtmosphericAttenuationDb(const float *attDb64, bool enable) {
        _mel.setAtmosphericAttenuationDb(attDb64, enable);
    }

    /** @brief Sound Level Meter (LAeq / SPL). */
    const SoundLevelMeter* getSLM() const { return &_slm; }

    /** @brief Время последнего сэмпла (мс) для Audio Liveness Watchdog. */
    uint32_t getLastSampleTimeMs() const;

    /** @brief Handle Ring Buffer для прямого доступа (Core 0). */
    RingbufHandle_t getRingBuffer() const;

    size_t getStackHighWaterMark() const {
        return _stackHighWaterMark.load(std::memory_order_acquire);
    }

private:
    /** Lock-free Ring Buffer (FreeRTOS Ringbuf, byte buffer mode, 64 КБ). */
    RingbufHandle_t _ringBuf;

    /** Мьютекс для телеметрии и gain (редкие операции, не в аудиопотоке). */
    SemaphoreHandle_t _telemetryMutex;

    float _gain;               ///< Программное усиление (default: 1.2)
    uint32_t _errorCount;      ///< Счётчик ошибок I2S
    XVF3800_I2C *_xvf = nullptr; ///< Указатель на XVF чип для аппаратного HPF
    std::atomic<bool> _running{false}; ///< Флаг работы задачи
    /** HPF mode 0..4 (XMOS table); soft path only if !_xvf */
    std::atomic<uint8_t>   _hpfMode{0};
    HighPassFilter         _softHpf{125.0f, (float)I2S_SAMPLE_RATE};
    AudioTelemetry _telemetry; ///< Буфер телеметрии
    MelSpectrogram _mel;       ///< MEL-спектрограмма
    SoundLevelMeter  _slm;     ///< A-weighted LAeq / SPL
    FreeRtosTaskHandshake _taskSync;
    std::atomic<size_t> _stackHighWaterMark{0};

    // Task-owned heap (freed in stop()/begin-fail only after i2s_driver_uninstall)
    int32_t *_taskDmaBuf = nullptr;
    int16_t *_taskMonoBuf = nullptr;
    int16_t *_taskMelBuf = nullptr;
    void releaseTaskHeap();

    // Мониторинг внутри audioProducerTask (сбрасываются при stop)
    uint32_t _lastTelemetryMs = 0;

    /**
     * @brief Конвертация 32-бит стерео I2S в 16-бит моно PCM.
     * @param i2sBuf  Входной I2S буфер (32 бита на канал, 2 канала).
     * @param samples Количество стерео-сэмплов во входном буфере.
     * @param pcmBuf  Выходной 16-бит моно буфер.
     * @param gain    Программное усиление.
     * @return Количество моно-сэмплов в выходном буфере.
     */
    static size_t convertI2SToMono(const int32_t *i2sBuf, size_t samples,
                                   int16_t *pcmBuf, float gain);

    /** @brief Тело задачи FreeRTOS на Core 1. */
    static void audioProducerTask(void *param);
};

#endif // AUDIO_PRODUCER_H