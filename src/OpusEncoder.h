/**
 * @file    OpusEncoder.h
 * @brief   Асинхронный Opus-кодировщик на Core 0.
 *
 * ## Назначение
 *
 * OpusEncoderTask запускает отдельную задачу FreeRTOS на Core 0, которая:
 *   1. Читает PCM-сэмплы из Ring Buffer (AudioProducer на Core 1)
 *   2. Кодирует их в Opus (fixed-point, через sh123/esp32_opus@1.0.3)
 *   3. Вызывает callback с готовым Opus-пакетом
 *
 * ## Архитектура
 *
 * ```
 * Core 1 (AudioProducer)                Core 0 (OpusEncoder)
 * ─────────────────────                ─────────────────────
 * I2S DMA → HPF                         encodeTask() [5]
 *   → xRingbufferSend ──┬──→ xRingbufferReceive
 *                        │     → opus_encode(320 samples)
 *   → MEL-спектрограмма  │     → callback(data, len, ts)
 *                        │
 *                        └──→ RTSP / MQTT / HTTP (др. задачи Core 0)
 * ```
 *
 * ## Зависимости
 *
 * - `sh123/esp32_opus@1.0.3` (PlatformIO registry, Arduino-совместимая, GPLv3)
 * - `AudioProducer` (кольцевой буфер PCM)
 * - FreeRTOS (задача, mutex)
 *
 * ## Параметры кодирования
 *
 * Определены в Config.h:
 *   - OPUS_SAMPLE_RATE  = 16000  (16 кГц)
 *   - OPUS_FRAME_SIZE   = 320    (20 мс при 16 кГц)
 *   - OPUS_BITRATE      = 24000  (24 кбит/с)
 *   - OPUS_COMPLEXITY   = 5      (баланс скорость/качество)
 *   - OPUS_ENABLED      = 1      (PlatformIO build_flags)
 *
 * ## Включение/выключение
 *
 * - `platformio.ini`: `build_flags = -DOPUS_ENABLED=1` (или 0)
 * - При OPUS_ENABLED=0 код компилируется как пустая заглушка без линковки Opus
 *
 * ## Потокобезопасность
 *
 * - Чтение Ring Buffer — lock-free (single-producer Core 1, single-consumer Core 0)
 * - Callback защищён мьютексом (_cbMutex)
 *
 * ## Производительность
 *
 * На ESP32-S3 @ 240 МГц, fixed-point Opus:
 *   - Кодирование 20 мс фрейма: ~4 мс (5x real-time)
 *   - Стек задачи: 3072 байт
 *   - Память Opus encoder state: ~30 КБ PSRAM
 *
 * @see docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef OPUS_ENCODER_H
#define OPUS_ENCODER_H

#if OPUS_ENABLED

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "opus.h"
#include "Config.h"
#include "AudioLifecycle.h"
#include "EncodedAudioFanout.h"
#include "PcmFrameAccumulator.h"
#include "FreeRtosTaskHandshake.h"

/** Предварительное объявление — OpusEncoderTask получает PCM из AudioProducer. */
class AudioProducer;

/**
 * @brief Сигнатура callback-функции для Opus-пакетов.
 *
 * Вызывается из задачи encodeTask на Core 0 при готовности очередного пакета.
 *
 * @param data         Указатель на Opus-данные (сырой битстрим)
 * @param len          Размер пакета в байтах
 * @param timestamp_ms Временная метка пакета (монотонно возрастающее смещение, мс)
 */
/**
 * @brief Задача кодирования аудиопотока в Opus на Core 0.
 *
 * ## Жизненный цикл
 *
 *   1. Создание: конструктор
 *   2. Инициализация: begin(producer) — создаёт кодировщик и задачу
 *   3. Работа: encodeTask — бесконечный цикл чтения PCM/кодирования/callback
 *   4. Остановка: stop() или деструктор — удаляет задачу и кодировщик
 *
 * ## Использование
 *
 * @code
 *   OpusEncoderTask encoder;
 *   encoder.setCallback([](const uint8_t *d, size_t len, uint32_t ts) {
 *       // отправить d по сети
 *   });
 *   encoder.begin(&audioProducer);
 * @endcode
 *
 * @note Задача работает на Core 0 с приоритетом 5. Не передавайте большие
 *       данные в колбэк — он вызывается в контексте задачи кодирования.
 */
class OpusEncoderTask : public AudioLifecycleConsumer {
public:
    OpusEncoderTask();
    ~OpusEncoderTask();

    /**
     * @brief Инициализация кодировщика и запуск задачи.
     * @param producer Указатель на AudioProducer (должен быть уже запущен).
     * @return true если кодировщик создан и задача запущена.
     */
    bool begin(AudioProducer *producer, EncodedAudioFanout *fanout);

    /** @brief Остановка задачи и освобождение ресурсов. */
    void stop();

    /** @return true если задача активна. */
    bool isRunning() const;
    void pauseAudio() override;
    void resumeAudio() override;

private:
    AudioProducer    *_producer;    ///< Поставщик PCM-сэмплов (Core 1)
    EncodedAudioFanout *_fanout;    ///< Независимые очереди encoded packet
    ::OpusEncoder    *_encoder;     ///< Состояние кодировщика Opus
    std::atomic<bool>  _running{false}; ///< Флаг работы задачи
    std::atomic<bool>  _paused{false};
    SemaphoreHandle_t _producerAccessMutex;
    PcmFrameAccumulator<OPUS_FRAME_SIZE,
                        OPUS_FRAME_SIZE + I2S_DMA_BUF_LEN> _pcmAccumulator;
    AudioFrameClock _frameClock;
    int16_t _readBuffer[I2S_DMA_BUF_LEN]{};
    FreeRtosTaskHandshake _taskSync;

    /** @brief Тело задачи FreeRTOS. */
    static void encodeTask(void *param);
};

#endif // OPUS_ENABLED
#endif // OPUS_ENCODER_H