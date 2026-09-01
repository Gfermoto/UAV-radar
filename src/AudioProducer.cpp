/**
 * @file    AudioProducer.cpp
 * @brief   Реализация захвата аудио с lock-free Ring Buffer и DMA в SRAM.
 *
 * УЛУЧШЕНИЯ Phase 1 Critical (2026-07-01):
 *   - DMA буфер во ВНУТРЕННЕЙ SRAM (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
 *   - xRingbuffer вместо StreamBuffer+Mutex (lock-free, без priority inversion)
 *   - lastSampleTimeMs для Audio Liveness Watchdog
 *
 * Источники: oboe.com, ESP32-A2DP BluetoothA2DPSinkQueued.cpp
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "AudioProducer.h"
#include "AudioConvert.h"
#include "XVF3800_I2C.h"
#include <esp_heap_caps.h>

/* Размер ring buffer в байтах */
#define RING_BUF_BYTES  (RING_BUFFER_SIZE * sizeof(int16_t))

AudioProducer::AudioProducer()
    : _ringBuf(nullptr)
    , _telemetryMutex(nullptr)
    , _gain(DEFAULT_GAIN)
    , _errorCount(0)
    , _running(false)
{
    memset(&_telemetry, 0, sizeof(_telemetry));
}

bool AudioProducer::begin() {
    if (_running) return true;
    if (_taskSync.handle()) return false;

    /* Lock-free Ring Buffer (byte buffer, без mutex) */
    if (!_ringBuf) {
        _ringBuf = xRingbufferCreate(RING_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
        if (!_ringBuf) {
            Serial.printf("[CORE1] Ошибка создания xRingbuffer (%d байт)\n", RING_BUF_BYTES);
            return false;
        }
    }

    if (!_telemetryMutex) {
        // Телеметрия/gain: Core1 пишет, Core0/WebUI читает — ring lock-free, поля под mutex.
        _telemetryMutex = xSemaphoreCreateMutex();
        if (!_telemetryMutex) {
            Serial.printf("[CORE1] Ошибка создания мьютекса телеметрии\n");
            return false;
        }
    }

    /* Настройка I2S */
    i2s_config_t i2sConfig = {};
    i2sConfig.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2sConfig.sample_rate          = I2S_SAMPLE_RATE;
    i2sConfig.bits_per_sample      = I2S_BITS_PER_SAMPLE;
    i2sConfig.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL2;
    i2sConfig.dma_buf_count        = I2S_DMA_BUF_COUNT;
    i2sConfig.dma_buf_len          = I2S_DMA_BUF_LEN;
    i2sConfig.use_apll             = true;
    i2sConfig.tx_desc_auto_clear   = false;
    i2sConfig.fixed_mclk           = 0;

    esp_err_t     err = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[CORE1] Ошибка I2S драйвера: %d\n", err);
        return false;
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.bck_io_num   = PIN_I2S_BCLK;
    pinConfig.ws_io_num    = PIN_I2S_WS;
    pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
    pinConfig.data_in_num  = PIN_I2S_DATA_IN;

    err = i2s_set_pin(I2S_NUM_0, &pinConfig);
    if (err != ESP_OK) {
        Serial.printf("[CORE1] Ошибка I2S пинов: %d\n", err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);

    if (!_mel.begin()) {
        Serial.printf("[CORE1] MEL init failed, continuing without spectrogram\n");
    }
    _slm.reset();

    if (!_taskSync.prepareStart()) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    _running = true;
    TaskHandle_t newHandle = nullptr;
    // Core 1 invariant: I2S/DMA не делит CPU с lwIP/WiFi (только Core 0).
    BaseType_t created = xTaskCreatePinnedToCore(
        audioProducerTask, "audioProducer",
        AUDIO_PRODUCER_STACK_SIZE, this,
        AUDIO_PRODUCER_PRIORITY, &newHandle, 1
    );

    if (created != pdPASS || !newHandle) {
        _running = false;
        _taskSync.abortStart();
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    _taskSync.publishHandle(newHandle);
    _taskSync.releaseTask();
    if (!_taskSync.waitStartup(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
        _running = false;
        _taskSync.requestStop();
        if (_taskSync.handle()) {
            if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 10))) {
                // Не ESP.restart(): иначе reboot-loop при залипшем I2S.
                // Escalate через SystemMonitor / Liveness failCount.
                Serial.printf("[CORE1] AudioProducer begin startup timeout — soft-fail\n");
                _taskSync.abortStart();
                i2s_driver_uninstall(I2S_NUM_0);
                releaseTaskHeap();
                return false;
            }
        }
        i2s_driver_uninstall(I2S_NUM_0);
        releaseTaskHeap();
        return false;
    }

    Serial.printf("[CORE1] AudioProducer: Core=1 Pri=%d Ring=xRingbuf(%d B) DMA=SRAM I2S=%dHz\n",
                  AUDIO_PRODUCER_PRIORITY, RING_BUF_BYTES, I2S_SAMPLE_RATE);
    return true;
}

void AudioProducer::releaseTaskHeap() {
    if (_taskMelBuf) {
        heap_caps_free(_taskMelBuf);
        _taskMelBuf = nullptr;
    }
    if (_taskMonoBuf) {
        heap_caps_free(_taskMonoBuf);
        _taskMonoBuf = nullptr;
    }
    if (_taskDmaBuf) {
        heap_caps_free(_taskDmaBuf);
        _taskDmaBuf = nullptr;
    }
}

void AudioProducer::stop() {
    _running = false;
    _taskSync.requestStop();
    if (_taskSync.handle()) {
        // Wait for task to leave i2s_read before uninstalling the driver.
        if (!_taskSync.waitExit(pdMS_TO_TICKS(TASK_STOP_GRACE_MS * 40))) {
            // Prefer reboot over vTaskDelete: async Core1 delete races I2S/DMA free.
            // Residual: persistent hang → reboot loop (SystemMonitor / scheduled reset).
            Serial.printf("[CORE1] AudioProducer stop timeout — rebooting\n");
#if !defined(UNIT_TEST)
            ESP.restart();
#endif
            return;
        }
    }
    // Stop DMA before freeing task buffers (never free while I2S may DMA).
    i2s_driver_uninstall(I2S_NUM_0);
    releaseTaskHeap();
    if (_ringBuf) {
        size_t itemSize = 0;
        void *item = nullptr;
        while ((item = xRingbufferReceiveUpTo(
                    _ringBuf, &itemSize, 0, RING_BUF_BYTES)) != nullptr) {
            vRingbufferReturnItem(_ringBuf, item);
        }
    }
    _lastTelemetryMs = 0;
}

size_t AudioProducer::readSamples(int16_t *buffer, size_t maxSamples) {
    if (!_ringBuf || !buffer || maxSamples == 0) return 0;

    size_t bytesNeeded = maxSamples * sizeof(int16_t);
    size_t itemSize = 0;

    /* Lock-free приём из Ring Buffer */
    uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(_ringBuf, &itemSize,
                                                       pdMS_TO_TICKS(10), bytesNeeded);
    if (!item || itemSize == 0) return 0;

    size_t samples = itemSize / sizeof(int16_t);
    if (samples > maxSamples) samples = maxSamples;
    memcpy(buffer, item, samples * sizeof(int16_t));

    /* Обязательный возврат буфера */
    vRingbufferReturnItem(_ringBuf, item);

    return samples;
}

size_t AudioProducer::available() const {
    if (!_ringBuf) return 0;
    size_t freeBytes = xRingbufferGetCurFreeSize(_ringBuf);
    if (freeBytes >= RING_BUF_BYTES) return 0;  // буфер пуст
    return (RING_BUF_BYTES - freeBytes) / sizeof(int16_t);
}

void AudioProducer::getTelemetry(AudioTelemetry &telemetry) {
    if (!_telemetryMutex) {
        memset(&telemetry, 0, sizeof(telemetry));
        return;
    }
    if (xSemaphoreTake(_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(&telemetry, &_telemetry, sizeof(AudioTelemetry));
        xSemaphoreGive(_telemetryMutex);
    }
}

RingbufHandle_t AudioProducer::getRingBuffer() const {
    return _ringBuf;
}

void AudioProducer::setGain(float gain) {
    if (!_telemetryMutex) return;
    if (xSemaphoreTake(_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        _gain = gain;
        xSemaphoreGive(_telemetryMutex);
    }
}

float AudioProducer::getGain() {
    float val = DEFAULT_GAIN;
    if (!_telemetryMutex) return val;
    if (xSemaphoreTake(_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        val = _gain;
        xSemaphoreGive(_telemetryMutex);
    }
    return val;
}

void AudioProducer::setXvfPointer(XVF3800_I2C *xvf) {
    _xvf = xvf;
    // Re-bind soft vs hardware path after pointer change.
    setHpfMode(_hpfMode.load(std::memory_order_relaxed));
}

void AudioProducer::setHpfMode(uint8_t mode) {
    if (mode > 4) mode = 2;
    _hpfMode.store(mode, std::memory_order_relaxed);
    // XMOS UG: 0=Off, 1=on70, 2=on125, 3=on150, 4=on180
    // Указатель на объект ≠ живой чип: при !isInitialized() — soft HPF.
    if (_xvf && _xvf->isInitialized()) {
        _xvf->setXvfHpfMode(mode);
        _softHpf.setEnabled(false);  // never double-filter with chip HPF
        return;
    }
    static const float kHz[5] = {0.0f, 70.0f, 125.0f, 150.0f, 180.0f};
    if (mode == 0) {
        _softHpf.setEnabled(false);
    } else {
        _softHpf.setCutoff(kHz[mode]);
        _softHpf.setEnabled(true);
    }
}

void AudioProducer::setHpfEnabled(bool enabled) {
    setHpfMode(enabled ? 2 : 0);
}

bool AudioProducer::isHpfEnabled() const {
    return _hpfMode.load(std::memory_order_relaxed) != 0;
}

uint8_t AudioProducer::getHpfMode() const {
    return _hpfMode.load(std::memory_order_relaxed);
}

float AudioProducer::getHpfCutoff() const {
    static const float kHz[5] = {0.0f, 70.0f, 125.0f, 150.0f, 180.0f};
    uint8_t m = getHpfMode();
    if (m > 4) m = 2;
    return kHz[m];
}

uint32_t AudioProducer::getLastSampleTimeMs() const {
    if (!_telemetryMutex) return 0;
    uint32_t value = 0;
    if (xSemaphoreTake(_telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        value = _telemetry.lastSampleTimeMs;
        xSemaphoreGive(_telemetryMutex);
    }
    return value;
}

/* ---- Приватные -------------------------------------------------------- */

size_t AudioProducer::convertI2SToMono(const int32_t *i2sBuf, size_t samples,
                                        int16_t *pcmBuf, float gain) {
    int32_t gainQ16 = static_cast<int32_t>(gain * 65536.0f);
    return audioConvertI2SToMono(i2sBuf, samples, pcmBuf, gainQ16, nullptr, nullptr);
}

void AudioProducer::audioProducerTask(void *param) {
    AudioProducer *self = static_cast<AudioProducer *>(param);
    self->_taskSync.waitForRelease();

    const size_t dmaReadSamples = I2S_DMA_BUF_LEN;

    /* КРИТИЧЕСКОЕ УЛУЧШЕНИЕ: DMA буфер во ВНУТРЕННЕЙ SRAM (не PSRAM!)
     * PSRAM слишком медленный для DMA и вызывает jitter.
     * Источник: oboe.com, ESP-IDF I2S Driver docs
     * Free: stop()/begin-fail after i2s_driver_uninstall — never before. */
    self->_taskDmaBuf = (int32_t *)heap_caps_malloc(
        dmaReadSamples * 2 * sizeof(int32_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!self->_taskDmaBuf) {
        Serial.printf("[CORE1] FATAL: DMA buf SRAM alloc failed (%zu B)\n",
                      dmaReadSamples * 2 * sizeof(int32_t));
        self->_running = false;
        self->_taskSync.signalStartup(false);
        self->_taskSync.signalExit();
        vTaskDelete(nullptr);
        return;
    }

    /* Конвертационный буфер – можно в PSRAM.
     * Размер = dmaReadSamples (число стерео-кадров I2S → столько же mono-сэмплов). */
    self->_taskMonoBuf = (int16_t *)heap_caps_malloc(
        dmaReadSamples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!self->_taskMonoBuf) {
        Serial.printf("[CORE1] FATAL: mono buf alloc failed\n");
        self->_running = false;
        self->_taskSync.signalStartup(false);
        self->_taskSync.signalExit();
        vTaskDelete(nullptr);
        return;
    }

    Serial.printf("[CORE1] Аудио-цикл: DMA=%zuB SRAM, Mono=%zuB PSRAM\n",
                  dmaReadSamples * 2 * sizeof(int32_t),
                  dmaReadSamples * sizeof(int16_t));

    size_t totalRead = 0;
    uint32_t errorCount = 0;
    uint32_t ringBufDropCount = 0;
    int64_t rmsAccumulator = 0;
    size_t rmsCount = 0;
    int32_t peakFrame = 0;

    // MEL: кольцевой буфер накопления PCM (1 фрейм = FFT size)
    size_t melPos = 0;
    self->_taskMelBuf = (int16_t *)heap_caps_calloc(
        MelSpectrogram::kFftSize, sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float melFrame[MelSpectrogram::kNumBands];

    if (!self->_taskMelBuf) {
        Serial.printf("[CORE1] FATAL: mel buf alloc failed\n");
        // Buffers freed by begin()/stop() after i2s_driver_uninstall.
        self->_running = false;
        self->_taskSync.signalStartup(false);
        self->_taskSync.signalExit();
        vTaskDelete(nullptr);
        return;
    }
    memset(self->_taskMelBuf, 0, MelSpectrogram::kFftSize * sizeof(int16_t));

    int32_t *dmaBuf = self->_taskDmaBuf;
    int16_t *monoBuf = self->_taskMonoBuf;
    int16_t *melBuf = self->_taskMelBuf;

    self->_taskSync.signalStartup(true);

    while (self->_running && !self->_taskSync.stopRequested()) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, dmaBuf,
                                  dmaReadSamples * 2 * sizeof(int32_t),
                                  &bytesRead, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            errorCount++;
            self->_errorCount = errorCount;
            uint32_t now_err = millis();
            if (xSemaphoreTake(self->_telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                if (now_err - self->_telemetry.lastLogTimeMs >= I2S_ERROR_LOG_INTERVAL_MS) {
                    Serial.printf("[CORE1] I2S err 0x%04X (%u total)\n", err, errorCount);
                    self->_telemetry.lastLogTimeMs = now_err;
                }
                xSemaphoreGive(self->_telemetryMutex);
            }
            continue;
        }

        // Проверка флага остановки после таймаута i2s_read
        if (!self->_running) break;

        size_t i2sSamplesRead = bytesRead / sizeof(int32_t);

        float currentGain;
        if (xSemaphoreTake(self->_telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            currentGain = self->_gain;
            xSemaphoreGive(self->_telemetryMutex);
        } else {
            currentGain = DEFAULT_GAIN;
        }

        size_t monoCount;
        {
            int32_t gainQ16 = static_cast<int32_t>(currentGain * 65536.0f);
            int64_t rmsAccBlock = 0;
            int32_t peakBlock = 0;
            monoCount = audioConvertI2SToMono(dmaBuf, i2sSamplesRead, monoBuf,
                                              gainQ16, &rmsAccBlock, &peakBlock);
            rmsAccumulator += rmsAccBlock;
            if (peakBlock > peakFrame) peakFrame = peakBlock;
        }

        // Диагностика: первый I2S-сэмпл (один раз при старте)
        static bool diag_done = false;
        if (!diag_done && i2sSamplesRead >= 2) {
            int32_t raw0 = dmaBuf[0];
            int32_t raw1 = dmaBuf[1];
            int32_t hi0 = raw0 >> 16;
            int32_t lo0 = raw0 & 0xFFFF;
            int32_t hi1 = raw1 >> 16;
            int32_t lo1 = raw1 & 0xFFFF;
            Serial.printf("[CORE1] I2S RAW: ch0=0x%08X ch1=0x%08X | hi0=%d lo0=%d | hi1=%d lo1=%d\n",
                (unsigned)raw0, (unsigned)raw1, hi0, lo0, hi1, lo1);
            diag_done = true;
        }

        // Soft HPF: нет живого чипа. С живым XVF soft выключен в setHpfMode.
        if (!self->_xvf || !self->_xvf->isInitialized()) {
            self->_softHpf.processBlock(monoBuf, monoCount);
        }

        /* SLM: A-weighted уровни на каждом блоке */
        self->_slm.processFrame(monoBuf, monoCount);

        /* MEL: каждый hop. */
        for (size_t i = 0; i < monoCount; i++) {
            melBuf[melPos++] = monoBuf[i];
            if (melPos >= MelSpectrogram::kFftSize) {
                self->_mel.computeFrame(melBuf, melFrame);
                self->_mel.pushFrame(melFrame);
                size_t remain = MelSpectrogram::kFftSize - MelSpectrogram::kHopLength;
                memmove(melBuf, melBuf + MelSpectrogram::kHopLength,
                        remain * sizeof(int16_t));
                melPos = remain;
            }
        }

        /* Lock-free запись в xRingbuffer; timeout=0 — drop, не блокировать I2S. */
        size_t bytesToSend = monoCount * sizeof(int16_t);
        BaseType_t sent = xRingbufferSend(self->_ringBuf, monoBuf, bytesToSend, 0);
        if (sent != pdTRUE) {
            ringBufDropCount++;
        }

        totalRead += monoCount;
        rmsCount += monoCount;
        bool clippingFrame = (peakFrame >= PCM_MAX_AMP);

        /* Телеметрия каждую секунду */
        uint32_t nowTelem = millis();
        if (nowTelem - self->_lastTelemetryMs >= 1000) {
            self->_lastTelemetryMs = nowTelem;
            self->_stackHighWaterMark.store(
                uxTaskGetStackHighWaterMark(nullptr), std::memory_order_release);
            if (xSemaphoreTake(self->_telemetryMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                self->_telemetry.peak       = peakFrame;
                self->_telemetry.clipping   = clippingFrame;
                self->_telemetry.overruns   = errorCount;
                self->_telemetry.ringBufferDrops = ringBufDropCount;
                self->_telemetry.lastSampleTimeMs = nowTelem;

                if (rmsCount > 0) {
                    self->_telemetry.rms = static_cast<int32_t>(
                        sqrt(static_cast<double>(rmsAccumulator) / rmsCount));
                }
                if (self->_telemetry.rms > 0) {
                    self->_telemetry.avgDb = 20.0f * log10f(
                        static_cast<float>(self->_telemetry.rms) / PCM_SCALE_F);
                }
                self->_telemetry.samplesRead = totalRead;
                self->_telemetry.samplesTotal += totalRead;
                self->_telemetry.confidence = 0.0f;
                self->_telemetry.durationMs = 0;
                totalRead = 0;

                xSemaphoreGive(self->_telemetryMutex);
            }
            rmsAccumulator = 0;
            rmsCount = 0;
            peakFrame = 0;
        }

        taskYIELD();
    }

    // Heap stays until stop() uninstalls I2S, then releaseTaskHeap().
    Serial.printf("[CORE1] AudioProducer завершён\n");
    self->_taskSync.signalExit();
    vTaskDelete(nullptr);
}
