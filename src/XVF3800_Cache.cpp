/**
 * @file    XVF3800_Cache.cpp
 * @brief   Реализация кэша данных XVF3800.
 */

#include "XVF3800_Cache.h"
#include "XVF3800_I2C.h"
#include <cstring>
#include <cmath>
#include <atomic>

namespace {
std::atomic<uint32_t> g_xvfPollGen{0};
std::atomic<uint32_t> g_xvfHeartGen{0};
}

uint32_t XVF3800_Cache::pollGen() { return g_xvfPollGen.load(); }
uint32_t XVF3800_Cache::heartGen() { return g_xvfHeartGen.load(); }

XVF3800_Cache::XVF3800_Cache(XVF3800_I2C *xvf)
    : _xvf(xvf)
    , _lastPollMs(0)
    , _lastHeavyMs(0)
    , _lastSlowMs(0)
    , _lastDoaAttemptMs(0)
    , _lastBusRecoverMs(0)
    , _doaFailStreak(0)
    , _heartFailStreak(0)
{
    memset(&_data, 0, sizeof(_data));
    _data.selectedAzimuthDeg = NAN;
    _data.rt60 = -1.0f;
}

void XVF3800_Cache::update() {
    g_xvfPollGen.fetch_add(1, std::memory_order_relaxed);
    if (!_xvf) return;

    const uint32_t now = millis();
    uint32_t lastXvfOkMs = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
    if (now - _lastPollMs < XVF_POLL_INTERVAL_MS) return;
        _lastPollMs = now;
        lastXvfOkMs = _data.lastXvfOkMs;
    }

    // Heavy heartbeat: energy+azimuths.
    // Если XVF ещё ни разу не OK — не долбить каждые 100 мс (Seeed → 0x40 busy).
    const bool needAlive = (lastXvfOkMs == 0);
    const uint32_t heartPeriodMs = needAlive ? 2000u : 500u;
    // AGC/sel/rt — отдельный таймер: раньше doSlow сверялся с _lastHeavyMs,
    // который сбрасывался каждые heartPeriodMs → AGC читался только 1 раз.
    const uint32_t slowPeriodMs = needAlive ? 4000u : 2000u;
    const bool doHeart =
        (_lastHeavyMs == 0) ||
        ((uint32_t)(now - _lastHeavyMs) >= heartPeriodMs);
    const bool doSlow =
        (_lastSlowMs == 0) ||
        ((uint32_t)(now - _lastSlowMs) >= slowPeriodMs);

    float gainDb = 0.0f;
    bool gotGain = false;
    float e0 = 0, e1 = 0, e2 = 0, e3 = 0;
    bool gotEnergy = false;
    float az[4] = {};
    bool gotAz = false;
    float proc = 0, asel = 0;
    bool procOk = false;
    bool gotSel = false;
    bool conv = false;
    bool gotConv = false;
    float rt = -1.0f;
    bool gotRt = false;
    int32_t atype = 0;
    bool gotType = false;

    if (doHeart) {
        _lastHeavyMs = now;
        g_xvfHeartGen.fetch_add(1, std::memory_order_relaxed);
        _diag.heartAttempts++;
        XVF3800_Result eRes = _xvf->readSpEnergy(e0, e1, e2, e3);
        gotEnergy = (eRes == XVF3800_Result::OK);
        _diag.lastEnergyRes = static_cast<uint8_t>(eRes);
        if (gotEnergy) _diag.energyOk++; else _diag.energyFail++;
        XVF3800_Result azRes = _xvf->readAzimuthsDeg(az);
        gotAz = (azRes == XVF3800_Result::OK);
        _diag.lastAzRes = static_cast<uint8_t>(azRes);
        if (gotAz) _diag.azOk++; else _diag.azFail++;
        // 0x40/0x41 = slave ответил «busy» — DSP жив, не считаем xvf_dead.
        const bool busBusy =
            (eRes == XVF3800_Result::ERR_BAD_STATUS) ||
            (azRes == XVF3800_Result::ERR_BAD_STATUS);
        if (doSlow) {
            _lastSlowMs = now;
            gotGain = (_xvf->readAGCGain(gainDb) == XVF3800_Result::OK);
            gotSel = (_xvf->readSelectedAzimuthsDeg(proc, asel, procOk) == XVF3800_Result::OK);
            gotConv = (_xvf->readAecConverged(conv) == XVF3800_Result::OK);
            gotRt = (_xvf->readRt60(rt) == XVF3800_Result::OK);
            gotType = (_xvf->readMicArrayType(atype) == XVF3800_Result::OK);
        }
        const bool heartOk = gotEnergy || gotAz || gotGain || gotSel ||
                             gotConv || gotRt || gotType;
        if (heartOk) {
            _heartFailStreak = 0;
        } else if (_heartFailStreak < 200) {
            _heartFailStreak++;
        }
        _diag.heartFailStreak = _heartFailStreak;
        // После серии fail — soft recover (ADDR ACK ≠ живой control protocol).
        if (!heartOk && _heartFailStreak >= 6 &&
            (uint32_t)(now - _lastBusRecoverMs) >= 15000u) {
            _lastBusRecoverMs = now;
            if (_xvf->recoverBus()) {
                _heartFailStreak = 0;
                gotEnergy = (_xvf->readSpEnergy(e0, e1, e2, e3) == XVF3800_Result::OK);
                gotAz = (_xvf->readAzimuthsDeg(az) == XVF3800_Result::OK);
            }
        }
        if (!heartOk && busBusy) {
            // Пометим «видели» без payload — VAD unavailable, не xvf_dead.
            std::lock_guard<std::mutex> lock(_mutex);
            if (_data.lastXvfOkMs == 0) {
                _data.lastXvfOkMs = now;
                Serial.printf("[I2C] XVF busy but present — mark seen\n");
            }
        }
    }

    if (!gotEnergy && !gotAz) {
        // Пустые лучи — не ждём DOA (только 0x40 busy).
        // Как только energy пришла хоть раз — DOA разблокирован.
        _diag.energyOk = 0;
    }

    // DOA: безусловно, каждые 100 мс. Fallback на AEC_AZIMUTH_VALUES при 0x41.
    XVF3800_Result doaRes = XVF3800_Result::ERR_TIMEOUT;
    uint16_t azimuth = 0;
    float confidence = 0.0f;
    const uint32_t doaBaseInterval = (uint32_t)XVF_POLL_INTERVAL_MS;
    const uint32_t doaInterval =
        (_doaFailStreak >= 20) ? 10000u : doaBaseInterval; // после 20 fail — реже, не 60 с
    const bool tryDoa =
        ((_lastDoaAttemptMs == 0) ||
         ((uint32_t)(now - _lastDoaAttemptMs) >= doaInterval));
    if (tryDoa) {
        _lastDoaAttemptMs = now;
        doaRes = _xvf->readDOA(azimuth, confidence);
        if (doaRes != XVF3800_Result::OK && _doaFailStreak < 3) {
            delay(2);
            doaRes = _xvf->readDOA(azimuth, confidence);
        }
        _diag.lastDoaRes = static_cast<uint8_t>(doaRes);
        if (doaRes == XVF3800_Result::OK) {
            _diag.doaOk++;
            _doaFailStreak = 0;
        } else {
            _diag.doaFail++;
            if (_doaFailStreak < 100) _doaFailStreak++;
            static uint32_t s_lastDoaFailLog = 0;
            static uint32_t s_doaFails = 0;
            s_doaFails++;
            if ((uint32_t)(now - s_lastDoaFailLog) >= 2000u) {
                Serial.printf("[I2C] DOA fail %s (x%lu, streak=%u, backoff=%lums)\n",
                              XVF3800_I2C::resultToString(doaRes),
                              (unsigned long)s_doaFails,
                              (unsigned)_doaFailStreak,
                              (unsigned long)doaInterval);
                s_lastDoaFailLog = now;
                s_doaFails = 0;
            }
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (doaRes == XVF3800_Result::OK) {
        _data.doaAzimuth     = azimuth;
        _data.speechDetected = confidence;
        _data.vadActive      = (confidence >= 0.5f);
        _data.lastUpdateMs   = now;
        _data.lastXvfOkMs    = now;
    } else if (gotAz) {
        // Seeed 1.0.4: DOA_VALUE часто 0x41 — primary = луч max(SPENERGY),
        // иначе auto-select / beam0 (азимуты живые даже при нулевой энергии).
        int best = 0;
        float bestE = e0;
        if (gotEnergy) {
            if (e1 > bestE) { best = 1; bestE = e1; }
            if (e2 > bestE) { best = 2; bestE = e2; }
            if (e3 > bestE) { best = 3; bestE = e3; }
        }
        float d = (gotEnergy && bestE >= 0.02f) ? az[best]
                  : (gotSel ? asel : az[0]);
        while (d < 0.0f) d += 360.0f;
        while (d >= 360.0f) d -= 360.0f;
        _data.doaAzimuth = static_cast<uint16_t>(d + 0.5f);
        _data.vadActive = false;
        _data.speechDetected = 0.0f;
    }
    if (gotGain) {
        _data.dspGainDb = gainDb;
        _data.lastXvfOkMs = now;
    }
    if (gotEnergy) {
        _data.spEnergy[0] = e0;
        _data.spEnergy[1] = e1;
        _data.spEnergy[2] = e2;
        _data.spEnergy[3] = e3;
        _data.lastXvfOkMs = now;
    }
    if (gotAz) {
        memcpy(_data.beamAzimuthDeg, az, sizeof(az));
        _data.lastXvfOkMs = now;
    }
    if (gotSel) {
        _data.selectedValid = procOk;
        _data.selectedAzimuthDeg = procOk ? proc : NAN;
        _data.autoSelectAzimuthDeg = asel;
        _data.lastXvfOkMs = now;
    }
    if (gotConv) {
        _data.aecConverged = conv;
        _data.lastXvfOkMs = now;
    }
    if (gotRt) {
        _data.rt60 = rt;
        _data.lastXvfOkMs = now;
    }
    if (gotType) {
        _data.micArrayType = atype;
        _data.lastXvfOkMs = now;
    }

    // XVF был жив, но сердцебиение упало полностью (DSP физически отвалился).
    // Сбрасываем latch → fail-closed восстанавливается.
    if (_data.lastXvfOkMs != 0 && _heartFailStreak >= 30) {
        _data.lastXvfOkMs = 0;
        _heartFailStreak = 0;
        Serial.printf("[I2C] XVF heartbeat lost — fail-closed restored\n");
    }
}

void XVF3800_Cache::getData(XVF3800_CacheData &data) const {
    std::lock_guard<std::mutex> lock(_mutex);
    data = _data;
}

bool XVF3800_Cache::isVADActive() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.vadActive;
}

float XVF3800_Cache::getDspGainDb() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.dspGainDb;
}

bool XVF3800_Cache::isDoaFresh(uint32_t maxAgeMs) const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_data.lastUpdateMs == 0) return false;
    return (uint32_t)(millis() - _data.lastUpdateMs) <= maxAgeMs;
}

bool XVF3800_Cache::hasSeenXvf() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _data.lastXvfOkMs != 0;
}

void XVF3800_Cache::markXvfSeen() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_data.lastXvfOkMs == 0) {
        _data.lastXvfOkMs = millis();
    }
}

bool XVF3800_Cache::isXvfAlive(uint32_t maxAgeMs) const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_data.lastXvfOkMs == 0) return false;
    return (uint32_t)(millis() - _data.lastXvfOkMs) <= maxAgeMs;
}

XVF3800_Cache::Diag XVF3800_Cache::getDiag() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _diag;
}
