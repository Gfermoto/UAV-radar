/**
 * @file    XVF3800_I2C.cpp
 * @brief   Реализация I2C-драйвера XVF3800.
 *
 * КРИТИЧЕСКИ: Wire.endTransmission(false) используется перед Wire.requestFrom()
 * для отправки RESTART вместо STOP на шине. Без этого XVF3800 не отвечает.
 *
 * _busMutex: sensorPoll, WebUI applyEngineerConfig и heartbeat — один Wire.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "XVF3800_I2C.h"
#include "NetConfig.h"
#include <cmath>
#include <cstring>

// =============================================================================
//  Публичные методы
// =============================================================================

bool XVF3800_I2C::begin() {
    {
        std::lock_guard<std::mutex> lock(_busMutex);
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_FREQ);
        Wire.setTimeOut(50);
    }

    if (!isConnected()) {
        Serial.printf("[I2C] XVF3800 не найден на 0x%02X (SDA=%d SCL=%d). Scan:\n",
                      XVF3800_I2C_ADDR, (int)PIN_I2C_SDA, (int)PIN_I2C_SCL);
        {
            std::lock_guard<std::mutex> lock(_busMutex);
            uint8_t found = 0;
            for (uint8_t addr = 1; addr < 127; ++addr) {
                Wire.beginTransmission(addr);
                if (Wire.endTransmission() == 0) {
                    Serial.printf("[I2C]   device @ 0x%02X\n", addr);
                    found++;
                }
            }
            if (!found) Serial.printf("[I2C]   (пусто)\n");
        }
        _initialized = false;
        return false;
    }

    _initialized = true;
    Serial.printf("[I2C] XVF3800 обнаружен на адресе 0x%02X, I2C freq=%d Hz\n",
                  XVF3800_I2C_ADDR, I2C_CLOCK_FREQ);

    uint8_t major = 0, minor = 0, patch = 0;
    if (readVersion(major, minor, patch) == XVF3800_Result::OK) {
        Serial.printf("[I2C] Версия прошивки XVF3800: v%d.%d.%d\n", major, minor, patch);
    }

    return true;
}

bool XVF3800_I2C::isConnected() {
    std::lock_guard<std::mutex> lock(_busMutex);
    Wire.beginTransmission(XVF3800_I2C_ADDR);
    uint8_t error = Wire.endTransmission();
    return (error == 0);
}

bool XVF3800_I2C::recoverBus() {
    Serial.printf("[I2C] bus recover: Wire.end/begin SDA=%d SCL=%d\n",
                  (int)PIN_I2C_SDA, (int)PIN_I2C_SCL);
    {
        std::lock_guard<std::mutex> lock(_busMutex);
        Wire.end();
    }
    delay(20);
    {
        std::lock_guard<std::mutex> lock(_busMutex);
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_FREQ);
        Wire.setTimeOut(50);
    }
    delay(10);
    const bool ok = isConnected();
    _initialized = ok;
    Serial.printf("[I2C] bus recover %s\n", ok ? "OK" : "FAIL");
    return ok;
}

XVF3800_Result XVF3800_I2C::readDOA(uint16_t &azimuth, float &confidence) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;

    // DOA_VALUE: 2×uint16 LE — [0]=azimuth 0..359, [1]=speech_detected 0|1
    uint8_t buf[4] = {0};
    XVF3800_Result res = readResponse(RESID_GPO_SERVICER, CMD_READ_DOA, 4, buf);

    if (res == XVF3800_Result::OK) {
        uint16_t az = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
        uint16_t speech = static_cast<uint16_t>(buf[2]) | (static_cast<uint16_t>(buf[3]) << 8);
        if (az > 359) az = az % 360;
        azimuth = az;
        confidence = (speech != 0) ? 1.0f : 0.0f;
    }

    return res;
}

XVF3800_Result XVF3800_I2C::readVAD(bool &active) {
    // Отдельного VAD cmd нет — speech_detected в DOA_VALUE[1].
    uint16_t az = 0;
    float conf = 0.0f;
    XVF3800_Result res = readDOA(az, conf);
    if (res == XVF3800_Result::OK) {
        active = (conf >= 0.5f);
    }
    return res;
}

XVF3800_Result XVF3800_I2C::writeMute(bool muted) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    // Seeed: GPO_WRITE_VALUE {pin, level}; pin 30 = X0D30 mute+LED (1=mute).
    uint8_t payload[2] = { XVF_GPO_MUTE_PIN, static_cast<uint8_t>(muted ? 1 : 0) };
    return writeCommand(RESID_GPO_SERVICER, CMD_GPO_WRITE_VALUE, payload, 2);
}

XVF3800_Result XVF3800_I2C::setLedRingEnabled(bool on) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    // GPO ResID 20 / LED_EFFECT(12): product on/off = DoA(4) / off(0) only.
    uint8_t effect = on ? XVF_LED_EFFECT_DOA : XVF_LED_EFFECT_OFF;
    return writeCommand(RESID_GPO_SERVICER, CMD_GPO_LED_EFFECT, &effect, 1);
}

XVF3800_Result XVF3800_I2C::setAGC(bool enable) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    return writePpInt32(PP_AGCONOFF, enable ? 1 : 0);
}

XVF3800_Result XVF3800_I2C::readAGCGain(float &gainDb) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[4] = {0};
    XVF3800_Result res = readResponse(RESID_PP, PP_AGCGAIN, 4, buf);
    if (res == XVF3800_Result::OK) {
        float lin = 0.0f;
        memcpy(&lin, buf, sizeof(lin));
        if (lin < 1e-6f) lin = 1e-6f;
        gainDb = 20.0f * log10f(lin);
    }
    return res;
}

XVF3800_Result XVF3800_I2C::setEchoSuppression(bool enable) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    return writePpInt32(PP_ECHOONOFF, enable ? 1 : 0);
}

XVF3800_Result XVF3800_I2C::setFixedBeam(XVF3800_BeamMode mode) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4] = {0};
    payload[0] = static_cast<uint8_t>(mode);
    return writeCommand(RESID_AEC, CMD_FIXEDBEAMSONOFF, payload, 4);
}

XVF3800_Result XVF3800_I2C::setXvfHpfMode(uint8_t mode) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    // XMOS UG v3.2.1: int32 0=Off, 1=on70, 2=on125, 3=on150, 4=on180.
    if (mode > 4) mode = 4;
    int32_t v = static_cast<int32_t>(mode);
    uint8_t payload[4];
    memcpy(payload, &v, sizeof(v));
    XVF3800_Result wr = writeCommand(RESID_AEC, CMD_HPFONOFF, payload, sizeof(payload));
    delay(5);
    uint8_t rb = 0xFF;
    XVF3800_Result rd = readXvfHpfMode(rb);
    Serial.printf("[HPF] set=%u write=%s readback=%u (%s)\n",
                  (unsigned)mode, resultToString(wr),
                  (unsigned)rb, resultToString(rd));
    return wr;
}

XVF3800_Result XVF3800_I2C::readXvfHpfMode(uint8_t &mode) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[4] = {0};
    XVF3800_Result res = readResponse(RESID_AEC, CMD_HPFONOFF, 4, buf);
    if (res == XVF3800_Result::OK) {
        int32_t v = 0;
        memcpy(&v, buf, sizeof(v));
        if (v < 0) v = 0;
        if (v > 4) v = 4;
        mode = static_cast<uint8_t>(v);
    }
    return res;
}

XVF3800_Result XVF3800_I2C::readVersion(uint8_t &major, uint8_t &minor, uint8_t &patch) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;

    uint8_t buf[3] = {0};
    XVF3800_Result res = readResponse(RESID_VERSION, CMD_READ_VERSION, 3, buf);

    if (res == XVF3800_Result::OK) {
        major = buf[0];
        minor = buf[1];
        patch = buf[2];
    }

    return res;
}

XVF3800_Result XVF3800_I2C::reboot() {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;

    _initialized = false;
    XVF3800_Result res = writeCommand(RESID_VERSION, CMD_REBOOT, nullptr, 0);
    if (res != XVF3800_Result::OK) {
        _initialized = true;
    }
    return res;
}

XVF3800_Result XVF3800_I2C::setEmphasis(uint8_t mode) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4] = {0};
    payload[0] = mode;
    return writeCommand(RESID_AEC, CMD_EMPHASIS, payload, 4);
}

XVF3800_Result XVF3800_I2C::setMicGain(float gain) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4];
    memcpy(payload, &gain, sizeof(gain));
    return writeCommand(RESID_AUDIO_MGR, CMD_MIC_GAIN, payload, sizeof(payload));
}

XVF3800_Result XVF3800_I2C::setSilenceLevel(float level) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[8];
    memcpy(payload, &level, sizeof(level));
    float cur = level * 0.05f;
    if (cur < 1e-6f) cur = 1e-6f;
    memcpy(payload + 4, &cur, sizeof(cur));
    return writeCommand(RESID_AEC, CMD_SILENCELEVEL, payload, sizeof(payload));
}

XVF3800_Result XVF3800_I2C::readSpEnergy(float &beam0, float &beam1, float &beam2, float &beam3) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[16] = {0};
    XVF3800_Result res = readResponse(RESID_AEC, CMD_SPENERGY, 16, buf);
    if (res == XVF3800_Result::OK) {
        memcpy(&beam0, buf,       sizeof(float));
        memcpy(&beam1, buf + 4,   sizeof(float));
        memcpy(&beam2, buf + 8,   sizeof(float));
        memcpy(&beam3, buf + 12,  sizeof(float));
    }
    return res;
}

XVF3800_Result XVF3800_I2C::readAzimuthsDeg(float deg[4]) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    if (!deg) return XVF3800_Result::ERR_BAD_LENGTH;
    uint8_t buf[16] = {0};
    XVF3800_Result res = readResponse(RESID_AEC, CMD_AZIMUTH, 16, buf);
    if (res == XVF3800_Result::OK) {
        float rad[4];
        memcpy(rad, buf, sizeof(rad));
        const float k = 180.0f / 3.14159265f;
        for (int i = 0; i < 4; i++) {
            float d = rad[i] * k;
            while (d < 0.0f) d += 360.0f;
            while (d >= 360.0f) d -= 360.0f;
            deg[i] = d;
        }
    }
    return res;
}

static float radToDeg360(float rad) {
    float d = rad * (180.0f / 3.14159265f);
    while (d < 0.0f) d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return d;
}

XVF3800_Result XVF3800_I2C::setAsrout(bool enable) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4] = {0};
    payload[0] = enable ? 1 : 0;
    return writeCommand(RESID_AEC, CMD_ASROUT, payload, 4);
}

XVF3800_Result XVF3800_I2C::setAsroutGain(float gain) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4];
    memcpy(payload, &gain, sizeof(gain));
    return writeCommand(RESID_AEC, CMD_ASROUTGAIN, payload, 4);
}

XVF3800_Result XVF3800_I2C::readSelectedAzimuthsDeg(float &processedDeg, float &autoSelectDeg,
                                                     bool &processedValid) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[8] = {0};
    XVF3800_Result res = readResponse(RESID_AUDIO_MGR, CMD_SELECTED_AZIMUTHS, 8, buf);
    if (res == XVF3800_Result::OK) {
        float rad[2];
        memcpy(rad, buf, sizeof(rad));
        processedValid = !isnan(rad[0]);
        processedDeg = processedValid ? radToDeg360(rad[0]) : 0.0f;
        autoSelectDeg = isnan(rad[1]) ? 0.0f : radToDeg360(rad[1]);
    }
    return res;
}

XVF3800_Result XVF3800_I2C::setFixedBeamAzimuthsDeg(float az0, float az1) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    const float k = 3.14159265f / 180.0f;
    float rad[2] = { az0 * k, az1 * k };
    uint8_t payload[8];
    memcpy(payload, rad, sizeof(rad));
    return writeCommand(RESID_AEC, CMD_FIXEDBEAMSAZ, payload, 8);
}

XVF3800_Result XVF3800_I2C::setFixedBeamGating(bool enable) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    // AEC cmds — int32 LE (как FIXEDBEAMSONOFF); gating только в Fixed mode.
    uint8_t payload[4] = {0};
    payload[0] = static_cast<uint8_t>(enable ? 1 : 0);
    return writeCommand(RESID_AEC, CMD_FIXEDBEAMSGATING, payload, 4);
}

XVF3800_Result XVF3800_I2C::setAttns(uint8_t mode, float nominal, float slope) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    XVF3800_Result r1 = writePpInt32(PP_ATTNS_MODE, mode ? 1 : 0);
    XVF3800_Result r2 = writePpFloat(PP_ATTNS_NOMINAL, nominal);
    XVF3800_Result r3 = writePpFloat(PP_ATTNS_SLOPE, slope);
    if (r1 != XVF3800_Result::OK) return r1;
    if (r2 != XVF3800_Result::OK) return r2;
    return r3;
}

XVF3800_Result XVF3800_I2C::setRefGain(float gain) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4];
    memcpy(payload, &gain, sizeof(gain));
    return writeCommand(RESID_AUDIO_MGR, CMD_REF_GAIN, payload, 4);
}

XVF3800_Result XVF3800_I2C::setSysDelay(int32_t samples) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4];
    memcpy(payload, &samples, sizeof(samples));
    return writeCommand(RESID_AUDIO_MGR, CMD_SYS_DELAY, payload, 4);
}

XVF3800_Result XVF3800_I2C::setOutputMux(uint8_t leftCat, uint8_t leftSrc,
                                          uint8_t rightCat, uint8_t rightSrc) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t leftPayload[2] = { leftCat, leftSrc };
    XVF3800_Result r = writeCommand(RESID_AUDIO_MGR, CMD_OP_L, leftPayload, 2);
    if (r != XVF3800_Result::OK) return r;
    uint8_t rightPayload[2] = { rightCat, rightSrc };
    return writeCommand(RESID_AUDIO_MGR, CMD_OP_R, rightPayload, 2);
}

XVF3800_Result XVF3800_I2C::readAecConverged(bool &converged) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[4] = {0};
    XVF3800_Result res = readResponse(RESID_AEC, CMD_AECCONVERGED, 4, buf);
    if (res == XVF3800_Result::OK) {
        int32_t v = 0;
        memcpy(&v, buf, sizeof(v));
        converged = (v != 0);
    }
    return res;
}

XVF3800_Result XVF3800_I2C::readRt60(float &seconds) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[4] = {0};
    XVF3800_Result res = readResponse(RESID_AEC, CMD_RT60, 4, buf);
    if (res == XVF3800_Result::OK) {
        memcpy(&seconds, buf, sizeof(seconds));
    }
    return res;
}

XVF3800_Result XVF3800_I2C::readMicArrayType(int32_t &type) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t buf[4] = {0};
    XVF3800_Result res = readResponse(RESID_AEC, CMD_MIC_ARRAY_TYPE, 4, buf);
    if (res == XVF3800_Result::OK) {
        memcpy(&type, buf, sizeof(type));
    }
    return res;
}

XVF3800_Result XVF3800_I2C::writePpFloat(uint8_t cmd, float value) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4];
    memcpy(payload, &value, sizeof(value));
    return writeCommand(RESID_PP, cmd, payload, sizeof(payload));
}

XVF3800_Result XVF3800_I2C::writePpInt32(uint8_t cmd, int32_t value) {
    if (!_initialized) return XVF3800_Result::ERR_NOT_INIT;
    uint8_t payload[4];
    memcpy(payload, &value, sizeof(value));
    return writeCommand(RESID_PP, cmd, payload, sizeof(payload));
}

bool XVF3800_I2C::applyEngineerConfig(const NetConfigData &cfg) {
    if (!_initialized) return false;

    bool ok = true;
    auto chk = [&](XVF3800_Result r) {
        if (r != XVF3800_Result::OK) ok = false;
        delay(3);  // Seeed: пауза между write, иначе последующие read → busy/timeout
    };

    chk(writePpInt32(PP_AGCONOFF, cfg.dspAgcEnabled ? 1 : 0));
    chk(writePpFloat(PP_AGCMAXGAIN, cfg.dspAgcMaxGain));
    chk(writePpFloat(PP_AGCDESIREDLEVEL, cfg.dspAgcDesiredLevel));
    chk(writePpFloat(PP_AGCTIME, cfg.dspAgcTime));
    chk(writePpFloat(PP_AGCFASTTIME, cfg.dspAgcFastTime));
    chk(writePpFloat(PP_AGCALPHASLOW, cfg.dspAgcAlphaSlow));
    chk(writePpFloat(PP_AGCALPHAFAST, cfg.dspAgcAlphaFast));
    chk(writePpInt32(PP_LIMITONOFF, cfg.dspLimiterEnabled ? 1 : 0));
    chk(writePpFloat(PP_LIMITPLIMIT, cfg.dspLimiterThreshold));
    chk(writePpFloat(PP_MIN_NS, cfg.dspNsStationary));
    chk(writePpFloat(PP_MIN_NN, cfg.dspNsNonStationary));
    chk(setEchoSuppression(cfg.echoSuppressionEnabled));

    // Аппаратный HPF XVF3800 AEC_HPFONOFF 0..4 (Off/70/125/150/180).
    chk(setXvfHpfMode(cfg.hpfMode <= 4 ? cfg.hpfMode : 2));

    // Pre-emphasis AEC отключаем — не искажаем спектр под нейросеть.
    chk(setEmphasis(0));

    chk(setMicGain(cfg.dspMicGain));

    {
        float sl = 1e-5f; // AEC_ENV_NOISY
        if (cfg.aecEnvMode == 0) sl = 1e-7f; // AEC_ENV_QUIET
        chk(setSilenceLevel(sl));
    }

    // ASROUT: 0=AEC residuals (monitor), 1=beam/ASR
    chk(setAsrout(cfg.asroutEnabled != 0));
    if (cfg.asroutEnabled) {
        chk(setAsroutGain(cfg.asroutGain));
    }

    // I2S L = processed auto-select beam; R = silence (mono берёт только L)
    chk(setOutputMux(XVF_OP_CAT_PROCESSED, XVF_OP_SRC_AUTOSELECT,
                     XVF_OP_CAT_SILENCE, 0));

    // Всегда Adaptive. FIXEDBEAMSGATING у XMOS работает только в Fixed —
    // глушение слабых лучей в Adaptive: GUI по SPENERGY + NS/ATTNS на аудио.
    chk(setFixedBeam(XVF3800_BeamMode::ADAPTIVE));
    (void)cfg.fixedBeamsEnabled;
    (void)cfg.fixedBeamGating;

    chk(setAttns(cfg.attnsMode, cfg.attnsNominal, cfg.attnsSlope));
    chk(setRefGain(cfg.refGain));
    chk(setSysDelay(cfg.sysDelaySamples));

    // AGC уже задан через PP_AGCONOFF выше — дублирующий legacy-путь убран.

    if (!ok) {
        Serial.printf("[I2C] applyEngineerConfig: partial failure\n");
    }
    return ok;
}

const char* XVF3800_I2C::resultToString(XVF3800_Result result) {
    switch (result) {
        case XVF3800_Result::OK:              return "OK";
        case XVF3800_Result::ERR_TIMEOUT:     return "TIMEOUT";
        case XVF3800_Result::ERR_NACK_ADDR:   return "NACK_ADDR";
        case XVF3800_Result::ERR_NACK_DATA:   return "NACK_DATA";
        case XVF3800_Result::ERR_BAD_STATUS:  return "BAD_STATUS";
        case XVF3800_Result::ERR_BAD_LENGTH:  return "BAD_LENGTH";
        case XVF3800_Result::ERR_NOT_INIT:    return "NOT_INIT";
        default:                              return "UNKNOWN";
    }
}

// =============================================================================
//  Приватные методы
// =============================================================================

XVF3800_Result XVF3800_I2C::writeCommand(uint8_t resid, uint8_t cmd,
                                          const uint8_t *payload, uint8_t length) {
    if (payload == nullptr && length > 0) {
        return XVF3800_Result::ERR_BAD_LENGTH;
    }
    std::lock_guard<std::mutex> lock(_busMutex);

    Wire.beginTransmission(XVF3800_I2C_ADDR);
    Wire.write(resid);
    Wire.write(cmd);
    Wire.write(length);

    for (uint8_t i = 0; i < length; i++) {
        Wire.write(payload[i]);
    }

    uint8_t error = Wire.endTransmission(true);

    switch (error) {
        case 0:  return XVF3800_Result::OK;
        case 1:  return XVF3800_Result::ERR_BAD_LENGTH;
        case 2:  return XVF3800_Result::ERR_NACK_ADDR;
        case 3:  return XVF3800_Result::ERR_NACK_DATA;
        case 5:  return XVF3800_Result::ERR_TIMEOUT;
        default: return XVF3800_Result::ERR_TIMEOUT;
    }
}

XVF3800_Result XVF3800_I2C::readResponse(uint8_t resid, uint8_t cmd,
                                          uint8_t expected, uint8_t *buffer) {
    if (buffer == nullptr || expected == 0) {
        return XVF3800_Result::ERR_BAD_LENGTH;
    }

    XVF3800_Result last = XVF3800_Result::ERR_TIMEOUT;
    // Seeed 1.0.4 часто отвечает 0x40 busy после applyEngineerConfig —
    // без retry SPENERGY/AZIMUTH «молчат», compass/BF мёртвые.
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) delay(3);
        std::lock_guard<std::mutex> lock(_busMutex);

        Wire.beginTransmission(XVF3800_I2C_ADDR);
        Wire.write(resid);
        Wire.write(cmd | I2C_READ_BIT);
        Wire.write(expected + 1);

        uint8_t error = Wire.endTransmission(false);
        if (error != 0) {
            switch (error) {
                case 1:  last = XVF3800_Result::ERR_BAD_LENGTH; break;
                case 2:  last = XVF3800_Result::ERR_NACK_ADDR; break;
                case 3:  last = XVF3800_Result::ERR_NACK_DATA; break;
                case 5:  last = XVF3800_Result::ERR_TIMEOUT; break;
                default: last = XVF3800_Result::ERR_TIMEOUT; break;
            }
        } else {
            size_t bytesRead = Wire.requestFrom(
                XVF3800_I2C_ADDR, static_cast<size_t>(expected + 1));
            if (bytesRead != static_cast<size_t>(expected + 1)) {
                last = XVF3800_Result::ERR_TIMEOUT;
            } else {
                uint8_t status = Wire.read();
                if (status != 0x00) {
                    static uint32_t s_lastErrLogMs = 0;
                    static uint32_t s_errBurst = 0;
                    s_errBurst++;
                    const uint32_t t = millis();
                    if ((uint32_t)(t - s_lastErrLogMs) >= 2000u) {
                        Serial.printf(
                            "[I2C] XVF3800 status=0x%02X (x%lu) resid=%u cmd=%u\n",
                            status, (unsigned long)s_errBurst, resid, cmd);
                        s_lastErrLogMs = t;
                        s_errBurst = 0;
                    }
                    last = XVF3800_Result::ERR_BAD_STATUS;
                } else {
                    bool okRead = true;
                    for (uint8_t i = 0; i < expected; i++) {
                        if (!Wire.available()) {
                            okRead = false;
                            break;
                        }
                        buffer[i] = Wire.read();
                    }
                    if (okRead) {
                        return XVF3800_Result::OK;
                    }
                    last = XVF3800_Result::ERR_BAD_LENGTH;
                }
            }
        }

        const bool retryable =
            (last == XVF3800_Result::ERR_BAD_STATUS) ||
            (last == XVF3800_Result::ERR_TIMEOUT) ||
            (last == XVF3800_Result::ERR_NACK_DATA);
        if (!retryable || attempt == 3) {
            break;
        }
        delay(4 + attempt * 4);
    }

    if (last == XVF3800_Result::ERR_TIMEOUT ||
        last == XVF3800_Result::ERR_NACK_ADDR) {
        static uint32_t s_lastToLog = 0;
        static uint32_t s_toBurst = 0;
        s_toBurst++;
        const uint32_t t = millis();
        if ((uint32_t)(t - s_lastToLog) >= 3000u) {
            Serial.printf("[I2C] read fail %s (x%lu) resid=%u cmd=%u\n",
                          resultToString(last), (unsigned long)s_toBurst,
                          resid, cmd);
            s_lastToLog = t;
            s_toBurst = 0;
        }
    }
    return last;
}
