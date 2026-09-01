/**
 * @file    XVF3800_Cache.h
 * @brief   Кэш телеметрии XVF3800 — единый I2C-опрос для всех потребителей.
 *
 * update() на Core 0: DOA/VAD (heartbeat), тяжёлые beams/energy, медленные AGC/RT60.
 * getData() под _mutex — снимок XVF3800_CacheData для WebUI/MQTT/telemetry.
 *
 * isDoaFresh / isXvfAlive — staleness по lastUpdateMs / lastXvfOkMs.
 * markXvfSeen() после успешного begin() — иначе needAlive долбит I2C каждые 100 мс.
 *
 * @see XVF3800_I2C.h, docs/ARCHITECTURE.md
 */

#ifndef XVF3800_CACHE_H
#define XVF3800_CACHE_H

#include <Arduino.h>
#include <mutex>
#include <cmath>
#include "Config.h"

class XVF3800_I2C;

/** Кэшированные данные телеметрии XVF3800. */
struct XVF3800_CacheData {
    uint16_t doaAzimuth;
    /** DOA_VALUE[1] speech_detected as 0.0|1.0 — не «уверенность азимута». */
    float    speechDetected;
    bool     vadActive;
    float    dspGainDb;       ///< PP_AGCGAIN → dB
    float    spEnergy[4];
    float    beamAzimuthDeg[4];
    float    selectedAzimuthDeg; ///< AUDIO_MGR processed DoA (или NAN)
    float    autoSelectAzimuthDeg;
    bool     selectedValid;
    bool     aecConverged;
    float    rt60;
    int32_t  micArrayType;    ///< 1=linear, 2=squarecular
    uint32_t lastUpdateMs;    ///< последний успешный DOA_VALUE (VAD/az)
    uint32_t lastXvfOkMs;     ///< любой успешный XVF read (link alive)
};

/** Макс. энергия лучей (прокси активности сигнала), clamp 0..1.
 *  Seeed SPENERGY часто сотни–тысячи (не 0..1) — log10-нормализация.
 *  Уже нормированные 0..1 оставляем как есть. */
inline float xvfNormSpEnergy(float e) {
    if (!(e > 0.0f)) return 0.0f;
    if (e <= 1.0f) return e;
    float n = log10f(e + 1.0f) / 4.0f;  // ~1.0 при e≈9999
    if (n > 1.0f) n = 1.0f;
    return n;
}

inline float xvfMaxSpEnergy(const XVF3800_CacheData &cd) {
    float eMax = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float n = xvfNormSpEnergy(cd.spEnergy[i]);
        if (n > eMax) eMax = n;
    }
    return eMax;
}

/** speech_detected из DOA_VALUE — speech-biased VAD флаг чипа. */
inline bool xvfSpeechDetected(const XVF3800_CacheData &cd) {
    return cd.speechDetected >= 0.5f;
}

class XVF3800_Cache {
public:
    explicit XVF3800_Cache(XVF3800_I2C *xvf);
    void update();
    void getData(XVF3800_CacheData &data) const;
    bool isVADActive() const;
    float getDspGainDb() const;
    /** true если последний успешный DOA_VALUE свежее maxAgeMs (VAD/speech). */
    bool isDoaFresh(uint32_t maxAgeMs = 2000) const;
    /** true если XVF хотя бы раз успешно ответил (beams/energy/…). */
    bool hasSeenXvf() const;
    /**
     * Зафиксировать «XVF видели» после успешного begin()/version.
     * Иначе needAlive долбит I2C каждые 100 мс → вечный 0x40 и ложный xvf_dead.
     */
    void markXvfSeen();
    /** true если последний успешный XVF read свежее maxAgeMs. */
    bool isXvfAlive(uint32_t maxAgeMs = 2000) const;

    struct Diag {
        uint32_t heartAttempts{0};
        uint32_t energyOk{0};
        uint32_t energyFail{0};
        uint32_t azOk{0};
        uint32_t azFail{0};
        uint32_t doaOk{0};
        uint32_t doaFail{0};
        uint8_t  lastEnergyRes{0};
        uint8_t  lastAzRes{0};
        uint8_t  lastDoaRes{0};
        uint8_t  heartFailStreak{0};
    };
    Diag getDiag() const;
    static uint32_t pollGen();
    static uint32_t heartGen();

 private:
    XVF3800_I2C    *_xvf;
    XVF3800_CacheData _data;
    uint32_t          _lastPollMs;
    uint32_t          _lastHeavyMs;
    uint32_t          _lastSlowMs;   ///< отдельно от heavy: иначе AGC читается 1×
    uint32_t          _lastDoaAttemptMs;
    uint32_t          _lastBusRecoverMs;
    uint8_t           _doaFailStreak;
    uint8_t           _heartFailStreak;
    Diag              _diag{};
    mutable std::mutex _mutex;
};

#endif // XVF3800_CACHE_H
