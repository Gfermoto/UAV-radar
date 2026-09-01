/**
 * @file    OpusEncoder.cpp
 * @brief   Реализация асинхронного Opus-кодировщика на Core 0.
 *
 * ## Детали реализации
 *
 * Использует sh123/esp32_opus (Arduino-совместимая версия Opus).
 * Кодировщик создаётся через opus_encoder_create() с параметрами:
 *   - OPUS_APPLICATION_AUDIO (оптимально для природы/речи, CELT-режим)
 *   - VBR (переменный битрейт)
 *   - DTX=0, FEC=0 (минимальные накладные расходы для реального времени)
 *
 * ## Пайплайн кодирования
 *
 *   1. Ожидание OPUS_FRAME_SIZE сэмплов (320 = 20 мс @ 16 кГц)
 *   2. Чтение из Ring Buffer через AudioProducer::readSamples()
 *   3. opus_encode() — кодирование 20 мс PCM в Opus-пакет
 *   4. Вызов callback с готовым пакетом
 *
 * Стек задачи: OPUS_TASK_STACK_SIZE (~48KB) — canary при меньшем размере.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#if OPUS_ENABLED

#include "OpusEncoder.h"
#include "AudioProducer.h"
#include <esp_heap_caps.h>

// ── Конструктор / Деструктор ──

OpusEncoderTask::OpusEncoderTask()
    : _producer(nullptr)
    , _fanout(nullptr)
    , _encoder(nullptr)
    , _running(false)
    , _paused(false)
    , _producerAccessMutex(nullptr)
    , _frameClock(OPUS_SAMPLE_RATE)
{}

OpusEncoderTask::~OpusEncoderTask() {
    stop();  // Гарантированное освобождение ресурсов
}

// ── Инициализация ──

bool OpusEncoderTask::begin(AudioProducer *producer, EncodedAudioFanout *fanout) {
    if (!producer || !fanout) return false;
    if (_running) return true;
    if (_taskSync.handle()) return false;
    _producer = producer;
    _fanout = fanout;

    // Сериализует encodeTask и pause/resume — иначе гонка readSamples vs clear.
    _producerAccessMutex = xSemaphoreCreateMutex();
    if (!_producerAccessMutex) {
        Serial.printf("[OPUS] Mutex alloc failed\n");
        return false;
    }

    // Создание кодировщика: 16 кГц, моно, AUDIO (CELT-оптимизация)
    int error;
    _encoder = opus_encoder_create(OPUS_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !_encoder) {
        Serial.printf("[OPUS] Encoder create failed: %d\n", error);
        if (_encoder) opus_encoder_destroy(_encoder);
        _encoder = nullptr;
        vSemaphoreDelete(_producerAccessMutex);
        _producerAccessMutex = nullptr;
        return false;
    }

    // Конфигурация кодировщика (см. Config.h для значений)
    opus_encoder_ctl(_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(_encoder, OPUS_SET_VBR(1));           // Переменный битрейт
    opus_encoder_ctl(_encoder, OPUS_SET_DTX(0));           // Без DTX (не речь)
    opus_encoder_ctl(_encoder, OPUS_SET_INBAND_FEC(0));    // Без FEC (локальная сеть)
    opus_encoder_ctl(_encoder, OPUS_SET_PACKET_LOSS_PERC(0));

    _running = true;
    _paused = false;
    _pcmAccumulator.clear();
    _frameClock.reset();

    if (!_taskSync.prepareStart()) {
        _running = false;
        opus_encoder_destroy(_encoder);
        _encoder = nullptr;
        vSemaphoreDelete(_producerAccessMutex);
        _producerAccessMutex = nullptr;
        return false;
    }

    TaskHandle_t newHandle = nullptr;
    // Core 0: тяжёлый Opus не конкурирует с I2S real-time на Core 1.
    BaseType_t ret = xTaskCreatePinnedToCore(
        encodeTask, "opus_encode", OPUS_TASK_STACK_SIZE, this, 5, &newHandle, 0);

    if (ret != pdPASS || !newHandle) {
        Serial.printf("[OPUS] Task create failed\n");
        _running = false;
        _taskSync.abortStart();
        opus_encoder_destroy(_encoder);
        _encoder = nullptr;
        vSemaphoreDelete(_producerAccessMutex);
        _producerAccessMutex = nullptr;
        return false;
    }
    _taskSync.publishHandle(newHandle);
    _taskSync.releaseTask();
    if (!_taskSync.waitStartup(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
        _running = false;
        _taskSync.requestStop();
        if (_taskSync.handle()) {
            _taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10));
        }
        if (_taskSync.canReleaseResources()) {
            opus_encoder_destroy(_encoder);
            _encoder = nullptr;
            vSemaphoreDelete(_producerAccessMutex);
            _producerAccessMutex = nullptr;
        }
        return false;
    }

    Serial.printf("[OPUS] started: %dHz %dbps frame=%d complexity=%d\n",
                  OPUS_SAMPLE_RATE, OPUS_BITRATE, OPUS_FRAME_SIZE, OPUS_COMPLEXITY);
    return true;
}

// ── Остановка ──

void OpusEncoderTask::stop() {
    _running = false;
    _taskSync.requestStop();

    if (_taskSync.handle()) {
        if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
            Serial.printf("[OPUS] stop timeout; resources retained\n");
            return;
        }
    }

    // Уничтожение кодировщика Opus
    if (_encoder) {
        opus_encoder_destroy(_encoder);
        _encoder = nullptr;
    }

    if (_producerAccessMutex) {
        vSemaphoreDelete(_producerAccessMutex);
        _producerAccessMutex = nullptr;
    }
}

bool OpusEncoderTask::isRunning() const {
    return _running;
}

void OpusEncoderTask::pauseAudio() {
    if (!_producerAccessMutex) {
        _paused = true;
        return;
    }
    xSemaphoreTake(_producerAccessMutex, portMAX_DELAY);
    _paused = true;
    _pcmAccumulator.clear();
    xSemaphoreGive(_producerAccessMutex);
}

void OpusEncoderTask::resumeAudio() {
    if (!_producerAccessMutex) {
        _paused = false;
        return;
    }
    xSemaphoreTake(_producerAccessMutex, portMAX_DELAY);
    // Сброс backlog: иначе catch-up storm крутит Core0 без IDLE0.
    _pcmAccumulator.clear();
    if (_producer) {
        while (_producer->available() > 0) {
            (void)_producer->readSamples(_readBuffer, I2S_DMA_BUF_LEN);
        }
    }
    _frameClock.reset();
    _paused = false;
    xSemaphoreGive(_producerAccessMutex);
}

// ── Задача кодирования (Core 0) ──

void OpusEncoderTask::encodeTask(void *param) {
    OpusEncoderTask *self = static_cast<OpusEncoderTask *>(param);
    self->_taskSync.waitForRelease();
    self->_taskSync.signalStartup(true);

    int16_t pcmBuf[OPUS_FRAME_SIZE];     // 320 сэмплов = 20 мс PCM
    uint8_t opusBuf[OPUS_FRAME_SIZE * 2]; // Opus: макс ~2 байта на сэмпл
    while (self && self->_running && !self->_taskSync.stopRequested()) {
        if (self->_paused) {
            // Сливать PCM, чтобы ring не рос и resume не получил шторм кадров.
            if (xSemaphoreTake(self->_producerAccessMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                if (self->_producer) {
                    while (self->_producer->available() > 0) {
                        (void)self->_producer->readSamples(
                            self->_readBuffer, I2S_DMA_BUF_LEN);
                    }
                }
                self->_pcmAccumulator.clear();
                xSemaphoreGive(self->_producerAccessMutex);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (xSemaphoreTake(self->_producerAccessMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }
        if (self->_paused) {
            xSemaphoreGive(self->_producerAccessMutex);
            continue;
        }
        if (self->_pcmAccumulator.pending() < (size_t)OPUS_FRAME_SIZE) {
            size_t avail = self->_producer->available();
            if (avail > 0) {
                size_t read = self->_producer->readSamples(
                    self->_readBuffer, I2S_DMA_BUF_LEN);
                if (read > 0 && !self->_pcmAccumulator.append(self->_readBuffer, read)) {
                    Serial.printf("[OPUS] PCM accumulator overflow: %u\n", (unsigned)read);
                }
            }
        }

        bool frameReady = self->_pcmAccumulator.popFrame(pcmBuf);
        xSemaphoreGive(self->_producerAccessMutex);
        if (!frameReady) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const uint32_t frameTimestamp =
            self->_frameClock.consumeFrame(OPUS_FRAME_SIZE);
        // Opus-кодирование (fixed-point)
        int len = opus_encode(self->_encoder, pcmBuf, OPUS_FRAME_SIZE,
                              opusBuf, sizeof(opusBuf));
        if (len < 0) {
            Serial.printf("[OPUS] encode error: %d\n", len);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        self->_fanout->publish(opusBuf, (size_t)len, frameTimestamp);
        // Без паузы backlog PCM крутит Core 0 без IDLE0 → task_wdt (IDLE0).
        // 1 ms на кадр (~20 ms аудио) сохраняет real-time с запасом.
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    self->_taskSync.signalExit();
    vTaskDelete(nullptr);
}

#else
// Заглушка — Opus выключен.
// Включить: OPUS_ENABLED=1 в platformio.ini build_flags + sh123/esp32_opus в lib_deps.
#endif // OPUS_ENABLED