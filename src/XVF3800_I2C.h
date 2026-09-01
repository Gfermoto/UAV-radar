/**
 * @file    XVF3800_I2C.h
 * @brief   I2C-драйвер голосового процессора XMOS XVF3800.
 *
 * Инкапсулирует низкоуровневый протокол обмена по I2C, предоставляет
 * высокоуровневые методы чтения телеметрии: DOA (направление), VAD (детекция
 * голоса), AGC (усиление), управление режимами DSP (beamforming, эхоподавление).
 *
 * Протокол:
 *   Запись:  [I2C_ADDR] [resid] [cmd] [len] [payload...]
 *   Чтение:  [I2C_ADDR] [resid] [cmd|0x80] [expected_len+1]
 *            → restart → [status_byte] [data...]
 *
 * КРИТИЧЕСКИ: используется Wire.endTransmission(false) перед Wire.requestFrom()
 * для отправки RESTART вместо STOP на шине I2C.
 *
 * Потокобезопасность: все I2C-операции (writeCommand/readResponse) под _busMutex.
 * Вызов только с Core 0; Core 1 не трогает I2C. Параллельные вызовы с разных
 * потоков Core 0 безопасны — mutex сериализует шину.
 *
 * @see docs/ARCHITECTURE.md, docs/LED.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef XVF3800_I2C_H
#define XVF3800_I2C_H

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <mutex>
#include "Config.h"

/**
 * @brief  Результат операции I2C.
 */
enum class XVF3800_Result : uint8_t {
    OK                = 0,   ///< Успех
    ERR_TIMEOUT       = 1,   ///< Таймаут на шине
    ERR_NACK_ADDR     = 2,   ///< NACK на адрес устройства
    ERR_NACK_DATA     = 3,   ///< NACK на данные
    ERR_BAD_STATUS    = 4,   ///< Некорректный статус-байт в ответе
    ERR_BAD_LENGTH    = 5,   ///< Некорректная ожидаемая длина
    ERR_NOT_INIT      = 6    ///< Драйвер не инициализирован
};

/**
 * @brief  Режим фиксированного луча.
 */
enum class XVF3800_BeamMode : uint8_t {
    ADAPTIVE = 0,     ///< Адаптивный beamforming (по умолчанию)
    FIXED    = 1      ///< Фиксированный луч
};

/** @brief Драйвер XVF3800: I2C host_control, телеметрия, DSP/LED. Core 0 only. */
struct NetConfigData;

class XVF3800_I2C {
public:
    /**
     * @brief  Инициализация драйвера и шины I2C.
     * @return true при успехе, false при ошибке инициализации.
     */
    bool begin();

    /** true после успешного begin() (чип найден); false если XVF отсутствует. */
    bool isInitialized() const {
        return _initialized.load(std::memory_order_relaxed);
    }

    /**
     * @brief  Проверка наличия устройства на шине.
     * @return true если устройство отвечает на свой адрес.
     */
    bool isConnected();

    /**
     * @brief  Soft-recover I2C после серии BAD_STATUS/timeout (Wire.end/begin).
     * @return true если устройство снова отвечает.
     */
    bool recoverBus();

    /**
     * @brief  Чтение DOA_VALUE: азимут 0–359° + speech_detected.
     * @param[out] azimuth     Азимут в градусах.
     * @param[out] confidence  1.0 если speech_detected, иначе 0.0
     *                         (отдельного float-confidence у XVF нет).
     */
    XVF3800_Result readDOA(uint16_t &azimuth, float &confidence);

    /**
     * @brief  Speech detected из DOA_VALUE (отдельного VAD-регистра нет).
     */
    XVF3800_Result readVAD(bool &active);

    /** Mute через GPO_WRITE_VALUE (ResID 20 / Cmd 1). */
    XVF3800_Result writeMute(bool muted);

    /**
     * @brief Mic RGB ring (Seeed WS2812×12) via GPO LED_EFFECT.
     * @param on false → effect 0 (off); true → effect 4 (DoA, factory default).
     * Does not set breath/rainbow/color — those override DoA.
     * @see docs/LED.md, CMD_GPO_LED_EFFECT in Config.h
     */
    XVF3800_Result setLedRingEnabled(bool on);

    /** AGC on/off через PP_AGCONOFF (ResID 17 / Cmd 10). */
    XVF3800_Result setAGC(bool enable);

    /**
     * @brief  Текущий AGC gain из PP_AGCGAIN (линейный) → dB (20·log10).
     */
    XVF3800_Result readAGCGain(float &gainDb);

    /** Эхоподавление через PP_ECHOONOFF (ResID 17 / Cmd 23). */
    XVF3800_Result setEchoSuppression(bool enable);

    /** Adaptive (0) / Fixed (1) через AEC_FIXEDBEAMSONOFF. */
    XVF3800_Result setFixedBeam(XVF3800_BeamMode mode);

    /** AEC_HPFONOFF: 0=Off, 1=70, 2=125, 3=150, 4=180 Гц (int32 LE). */
    XVF3800_Result setXvfHpfMode(uint8_t mode);
    /** Read-back AEC_HPFONOFF (int32 LE → 0..4). */
    XVF3800_Result readXvfHpfMode(uint8_t &mode);
    XVF3800_Result readVersion(uint8_t &major, uint8_t &minor, uint8_t &patch);
    XVF3800_Result setEmphasis(uint8_t mode);
    XVF3800_Result setMicGain(float gain);
    XVF3800_Result setSilenceLevel(float level);
    XVF3800_Result readSpEnergy(float &beam0, float &beam1, float &beam2, float &beam3);
    XVF3800_Result readAzimuthsDeg(float deg[4]);

    /** ASROUT: 0=AEC residuals, 1=beam/ASR output на I2S. */
    XVF3800_Result setAsrout(bool enable);
    XVF3800_Result setAsroutGain(float gain);

    /**
     * @brief  AUDIO_MGR_SELECTED_AZIMUTHS → градусы.
     * @param[out] processedDeg  Processed DoA (NAN → valid=false).
     * @param[out] autoSelectDeg Auto-select beam DoA.
     * @param[out] processedValid false если processed был NAN.
     */
    XVF3800_Result readSelectedAzimuthsDeg(float &processedDeg, float &autoSelectDeg,
                                           bool &processedValid);

    XVF3800_Result setFixedBeamAzimuthsDeg(float az0, float az1);
    XVF3800_Result setFixedBeamGating(bool enable);
    XVF3800_Result setAttns(uint8_t mode, float nominal, float slope);
    XVF3800_Result setRefGain(float gain);
    XVF3800_Result setSysDelay(int32_t samples);

    /** I2S mux: category + source (см. XMOS Table 26). */
    XVF3800_Result setOutputMux(uint8_t leftCat, uint8_t leftSrc,
                                uint8_t rightCat, uint8_t rightSrc);

    XVF3800_Result readAecConverged(bool &converged);
    XVF3800_Result readRt60(float &seconds);
    XVF3800_Result readMicArrayType(int32_t &type);

    XVF3800_Result reboot();
    bool applyEngineerConfig(const NetConfigData &cfg);
    static const char* resultToString(XVF3800_Result result);

private:
    std::atomic<bool> _initialized{false};
    mutable std::mutex _busMutex;

    /**
     * @brief  Универсальная запись команды в XVF3800.
     * @param  resid    Идентификатор ресурса.
     * @param  cmd      Код команды.
     * @param  payload  Указатель на данные (может быть nullptr при length=0).
     * @param  length   Длина payload в байтах.
     * @return XVF3800_Result::OK при успехе.
     */
    XVF3800_Result writeCommand(uint8_t resid, uint8_t cmd,
                                const uint8_t *payload, uint8_t length);

    /**
     * @brief  Универсальное чтение ответа от XVF3800.
     * @param  resid     Идентификатор ресурса.
     * @param  cmd       Код команды (read bit добавится автоматически).
     * @param  expected  Ожидаемое количество байт данных.
     * @param[out] buffer Буфер для данных (должен быть >= expected).
     * @return XVF3800_Result::OK при успехе.
     */
    XVF3800_Result readResponse(uint8_t resid, uint8_t cmd,
                                uint8_t expected, uint8_t *buffer);

    XVF3800_Result writePpFloat(uint8_t cmd, float value);
    XVF3800_Result writePpInt32(uint8_t cmd, int32_t value);
};

#endif // XVF3800_I2C_H
