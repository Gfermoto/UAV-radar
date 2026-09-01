/**
 * @file    TelemetryBuilder.cpp
 * @brief   Сборка JSON телеметрии/событий/status для MQTT и WebUI.
 */

#include "TelemetryBuilder.h"
#include "Config.h"
#include "AudioProducer.h"
#include "AudioTelemetry.h"
#include "DspLevelCompensate.h"
#include "XVF3800_Cache.h"
#include "NTPClient.h"
#include "RTSPClient.h"
#include "SoundLevelMeter.h"
#include "WebCredentials.h"
#include <WiFi.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <atomic>
#include <string.h>

AudioProducer  *TelemetryBuilder::_producer  = nullptr;
XVF3800_Cache  *TelemetryBuilder::_xvfCache  = nullptr;
NTPClient      *TelemetryBuilder::_ntp       = nullptr;
RTSPClient     *TelemetryBuilder::_rtspClient = nullptr;
char             TelemetryBuilder::_nodeId[NODE_ID_LEN] = {0};

// Calibration: 0.0 = uncalibrated (no SPL fields emitted).
// Set via setCalibrationOffsetDb() from Config.
static std::atomic<float> s_calibrationOffsetDb{0.0f};
static std::atomic<uint32_t> s_loopGen{0};

void TelemetryBuilder::noteLoopTick() {
    s_loopGen.fetch_add(1, std::memory_order_relaxed);
}

uint32_t TelemetryBuilder::loopGen() {
    return s_loopGen.load(std::memory_order_relaxed);
}
static std::atomic<bool> s_dspAgcEnabled{true};
static std::atomic<bool> s_dspLimiterEnabled{true};
static std::atomic<uint8_t> s_aecEnvMode{1};  // AEC_ENV_NOISY
static std::atomic<bool> s_echoSuppression{false};
static std::atomic<uint8_t> s_asroutEnabled{1};
static std::atomic<bool> s_loudspeakerPresent{false};
static std::atomic<float> s_dspMicGain{10.0f};
static std::atomic<float> s_asroutGain{1.0f};
static std::atomic<uint8_t> s_attnsMode{1};
static std::atomic<float> s_attnsNominal{1.0f};
static std::atomic<float> s_attnsSlope{0.2f};
static std::atomic<uint8_t> s_ledMode{0};  // LED_MODE_STATUS
static std::atomic<uint8_t> s_localRtspClients{0};
static std::atomic<bool> s_audioSetupMode{false};
static std::atomic<uint32_t> s_sensorGen{0};
static std::atomic<int> s_resetReason{0};
static char s_lastEvent[32] = {0};

void TelemetryBuilder::setResetInfo(int reason, const char *event) {
    s_resetReason.store(reason, std::memory_order_relaxed);
    if (event) {
        strlcpy(s_lastEvent, event, sizeof(s_lastEvent));
    }
}
int TelemetryBuilder::resetReason() { return s_resetReason.load(std::memory_order_relaxed); }
const char *TelemetryBuilder::lastEvent() { return s_lastEvent; }

void TelemetryBuilder::setLocalRtspClientCount(uint8_t n) {
    s_localRtspClients.store(n, std::memory_order_relaxed);
}

void TelemetryBuilder::setAudioSetupMode(bool on) {
    s_audioSetupMode.store(on, std::memory_order_relaxed);
}

bool TelemetryBuilder::audioSetupMode() {
    return s_audioSetupMode.load(std::memory_order_relaxed);
}

void TelemetryBuilder::noteSensorTick() {
    s_sensorGen.fetch_add(1, std::memory_order_relaxed);
}

uint32_t TelemetryBuilder::sensorGen() {
    return s_sensorGen.load(std::memory_order_relaxed);
}

void TelemetryBuilder::init(AudioProducer *producer, XVF3800_Cache *xvfCache,
                             NTPClient *ntp, RTSPClient *rtspClient,
                             const char *nodeId) {
    _producer   = producer;
    _xvfCache   = xvfCache;
    _ntp        = ntp;
    _rtspClient = rtspClient;
    if (nodeId) {
        strncpy(_nodeId, nodeId, sizeof(_nodeId) - 1);
        _nodeId[sizeof(_nodeId) - 1] = '\0';
    }
}

// ---------------------------------------------------------------------------
//  Shared helpers
// ---------------------------------------------------------------------------

static char *jsonToAlloc(JsonDocument &doc, size_t *outLen,
                         TelemetryBuilder::AllocFn alloc) {
    const size_t need = measureJson(doc) + 1;
    if (need < 2) return nullptr;
    // alloc=nullptr → DRAM; WebUI передаёт webUiAlloc (PSRAM first) — беречь internal heap.
    char *buf = static_cast<char *>(alloc ? alloc(need) : malloc(need));
    if (!buf) return nullptr;
    serializeJson(doc, buf, need);
    if (outLen) *outLen = need - 1;
    return buf;
}

static String adoptMallocString(char *buf) {
    if (!buf) return String("{}");
    String out(buf);
    free(buf);
    return out;
}

static void writeTimeObject(JsonObject &dest, NTPClient *ntp) {
    const bool synced = ntp && ntp->isSynced();
    dest["synced"] = synced;
    if (synced) {
        int32_t age = ntp->getSyncAgeMs();
        dest["age_ms"] = age >= 0 ? age : 0;
        dest["uncertainty_ms"] = (int32_t)50;
    } else {
        dest["age_ms"] = -1;
        dest["uncertainty_ms"] = -1;
    }
}

static void writeAudioLevels(JsonObject &audio, const AudioTelemetry &telem,
                             float effectiveGainDb) {
    float peakdB = (telem.peak > 0)
        ? 20.0f * log10f((float)telem.peak / 32767.0f)
        : -96.0f;
    float rmsdB = (telem.rms > 0)
        ? 20.0f * log10f((float)telem.rms / 32767.0f)
        : -96.0f;

    peakdB = dspCompensateLevelDb(peakdB, effectiveGainDb);
    rmsdB = dspCompensateLevelDb(rmsdB, effectiveGainDb);

    audio["peak_level"] = peakdB;
    audio["rms_db"]     = rmsdB;
    audio["peak"]       = telem.peak;
    audio["rms"]        = telem.rms;
    audio["avg_db"]     = rmsdB;
    audio["clipping"]   = telem.clipping;
    audio["calibration_offset_db"] = s_calibrationOffsetDb.load();
    audio["dsp_effective_gain_db"] = effectiveGainDb;
}

static void writeSPL(JsonObject &audio, const AudioTelemetry &telem,
                     AudioProducer *producer, float effectiveGainDb) {
    const float calOffset = s_calibrationOffsetDb.load();
    const bool cal = fabsf(calOffset) >= 0.001f;

    const SoundLevelMeter *slm = producer ? producer->getSLM() : nullptr;
    if (!slm) return;

    float laeq = slm->getLAeq(), fast = slm->getFastSPL(), slow = slm->getSlowSPL();
    // I2S-referred (до AGC-компенсации) — для gate тишина/звук на DEV.
    audio["spl_fast_raw"] = fast;
    audio["spl_slow_raw"] = slow;
    audio["laeq_raw"]     = laeq;
    // acoustic ≈ raw − (AGC + ATTNS + mic + ASROUT)
    laeq = dspCompensateLevelDb(laeq, effectiveGainDb);
    fast = dspCompensateLevelDb(fast, effectiveGainDb);
    slow = dspCompensateLevelDb(slow, effectiveGainDb);
    if (cal) {
        laeq += calOffset;
        fast += calOffset;
        slow += calOffset;
    }
    audio["laeq_db"]  = laeq;
    audio["spl_fast"] = fast;
    audio["spl_slow"] = slow;
    audio["calibrated"] = cal;
}

static void writeSystem(JsonObject &sys) {
    sys["temp_c"]     = temperatureRead();
    sys["cpu_mhz"]    = (int)(ESP.getCpuFreqMHz());
    sys["wifi_rssi"]  = (int)WiFi.RSSI();
    sys["free_heap"]  = ESP.getFreeHeap();
    sys["free_heap_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    sys["free_heap_min"]  = ESP.getMinFreeHeap();
    sys["uptime_s"]   = millis() / 1000;
    sys["reset_reason"] = TelemetryBuilder::resetReason();
    sys["last_event"] = TelemetryBuilder::lastEvent();
}

static void writeExtendedFields(DynamicJsonDocument &doc, AudioProducer *producer,
                                XVF3800_Cache *xvfCache, NTPClient *ntp,
                                RTSPClient *rtspClient,
                                const AudioTelemetry &telem,
                                int doaDeg, float doaConf) {
    doc["uptime"]          = millis() / 1000;
    doc["ntp_synced"]      = ntp ? ntp->isSynced() : false;
    doc["rtsp_streaming"]  = s_localRtspClients.load() > 0;
    doc["audio_setup_mode"] = s_audioSetupMode.load();
    doc["rtsp_remote"]     = rtspClient ? rtspClient->isStreaming() : false;
    doc["sensor_gen"]      = TelemetryBuilder::sensorGen();
    doc["loop_gen"]        = TelemetryBuilder::loopGen();
    doc["wifi_ssid"]       = WiFi.SSID();
    doc["wifi_ip"]         = WiFi.localIP().toString();
    doc["wifi_mac"]        = WiFi.macAddress();
    const bool wifiOk = (WiFi.status() == WL_CONNECTED);
    doc["wifi_connected"]  = wifiOk;
    if (wifiOk) doc["wifi_rssi"] = (int)WiFi.RSSI();
    doc["free_heap"]       = ESP.getFreeHeap();
    // Top-level: WebUI читает d.free_heap_block (system.* в WS часто не разворачивают).
    doc["free_heap_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    doc["free_heap_min"]   = ESP.getMinFreeHeap();
    doc["free_psram"]      = ESP.getFreePsram();
    doc["temp"]            = temperatureRead();
    doc["audio_hpf"]       = producer ? producer->isHpfEnabled() : false;
    doc["audio_hpf_mode"]  = producer ? (int)producer->getHpfMode() : 0;
    doc["audio_hpf_cutoff_hz"] = producer ? producer->getHpfCutoff() : 0.0f;
    doc["dsp_agc_enabled"] = s_dspAgcEnabled.load();
    doc["dsp_limiter_enabled"] = s_dspLimiterEnabled.load();
    doc["aec_env"] = (int)s_aecEnvMode.load();
    doc["echo_suppression"] = s_echoSuppression.load();
    doc["asrout"] = (int)s_asroutEnabled.load();
    doc["loudspeaker_present"] = s_loudspeakerPresent.load();
    doc["dsp_mic_gain"] = s_dspMicGain.load();

    bool vadActive = false;
    float dspGainDb = 0.0f;
    float effectiveGainDb = 0.0f;
    if (xvfCache) {
        XVF3800_CacheData cacheData;
        xvfCache->getData(cacheData);
        vadActive = cacheData.vadActive;
        dspGainDb = cacheData.dspGainDb;
        DspLevelCompParams gp{};
        gp.agcGainDb = cacheData.dspGainDb;
        gp.agcEnabled = s_dspAgcEnabled.load();
        gp.micGainLin = s_dspMicGain.load();
        gp.asroutGainLin = s_asroutGain.load();
        gp.asroutEnabled = s_asroutEnabled.load() != 0;
        gp.attnsMode = s_attnsMode.load();
        gp.attnsNominal = s_attnsNominal.load();
        gp.attnsSlope = s_attnsSlope.load();
        gp.speechActive = xvfSpeechDetected(cacheData) || cacheData.vadActive;
        gp.agcGainInitLin = DSP_AGC_GAIN_INIT_LIN;
        gp.softwareGainLin = producer ? producer->getGain() : 1.0f;
        effectiveGainDb = dspEffectiveGainDb(gp);
    }
    doc["dsp_gain_db"] = dspGainDb;
    doc["dsp_effective_gain_db"] = effectiveGainDb;
    doc["vad"] = vadActive;
    doc["opus"] = true;

    // Speech energy per beam (AEC_SPENERGY_VALUES) — для верификации DOA
    if (xvfCache) {
        XVF3800_CacheData cd;
        xvfCache->getData(cd);
        JsonArray spEn = doc["sp_energy"].to<JsonArray>();
        for (int i = 0; i < 4; i++) spEn.add(xvfNormSpEnergy(cd.spEnergy[i]));
    }

    doc["led_mode"]         = s_ledMode.load();

    // as<> — не to<>: to<> очищает объект и стирает rms/SPL из base build.
    JsonObject audio = doc["audio"].as<JsonObject>();
    if (audio.isNull()) {
        audio = doc["audio"].to<JsonObject>();
    }
    audio["sample_rate"]   = I2S_SAMPLE_RATE;

    // Extended-only audio fields (SPL already in base build)
    const SoundLevelMeter *slm = producer ? producer->getSLM() : nullptr;
    if (slm) {
        audio["raw_rms"]   = slm->getRawRMS();
        audio["samples_processed"] = slm->getSamplesProcessed();
    }

    if (producer) {
        audio["hpf"]          = producer->isHpfEnabled();
        audio["hpf_mode"]     = (int)producer->getHpfMode();
        audio["hpf_cutoff_hz"] = producer->getHpfCutoff();
    }
    if (xvfCache) {
        audio["dsp_gain_db"]  = xvfCache->getDspGainDb();
    }
    audio["dsp_mic_gain"] = s_dspMicGain.load();
    audio["dsp_mic_gain"] = s_dspMicGain.load();

    // MEL metadata from the audio capture path.
    if (producer) {
        const MelSpectrogram *mel = producer->getMelSpectrogram();
        if (mel && mel->isReady()) {
            int fc = mel->getFrameCount();
            audio["mel_frames"] = fc;
            // Ловим NaN: первый MEL-фрейм
            float snap[64];
            if (mel->copyRecentFrames(snap, 1)) {
                // битовая карта: 0 если NaN, 1 если ok
                float check = snap[0];
                audio["mel_sample0"] = check;
                audio["mel_nan0"] = (check != check); // NaN check
            }
        }
    }
    JsonObject doaObj = doc["doa"].to<JsonObject>();
    doaObj["azimuth"]   = doaDeg;       // wind-corrected primary
    if (xvfCache) {
        XVF3800_CacheData cd;
        xvfCache->getData(cd);
        // speech_detected — отдельное поле (не «уверенность азимута»)
        doaObj["speech_detected"] = xvfSpeechDetected(cd);
        // confidence: энергия лучей (прокси активности сигнала), не VAD речи
        doaObj["confidence"] = xvfMaxSpEnergy(cd);
        doaObj["azimuth_raw"] = (int)cd.doaAzimuth;
        JsonArray beams = doaObj["beams"].to<JsonArray>();
        beams.add(cd.beamAzimuthDeg[0]);
        beams.add(cd.beamAzimuthDeg[1]);
        JsonArray azAll = doaObj["azimuths"].to<JsonArray>();
        for (int i = 0; i < 4; i++) azAll.add(cd.beamAzimuthDeg[i]);
        if (cd.selectedValid) doaObj["selected"] = cd.selectedAzimuthDeg;
        else doaObj["selected"] = nullptr;
        doaObj["auto_select"] = cd.autoSelectAzimuthDeg;
        doc["aec_converged"] = cd.aecConverged;
        doc["rt60"] = cd.rt60;
        doc["mic_array_type"] = cd.micArrayType;
        {
            const XVF3800_Cache::Diag dg = xvfCache->getDiag();
            JsonObject xd = doc["xvf_diag"].to<JsonObject>();
            xd["heart"] = dg.heartAttempts;
            xd["e_ok"] = dg.energyOk;
            xd["e_fail"] = dg.energyFail;
            xd["e_last"] = dg.lastEnergyRes;
            xd["az_ok"] = dg.azOk;
            xd["az_fail"] = dg.azFail;
            xd["az_last"] = dg.lastAzRes;
            xd["doa_ok"] = dg.doaOk;
            xd["doa_fail"] = dg.doaFail;
            xd["doa_last"] = dg.lastDoaRes;
            xd["fail_streak"] = dg.heartFailStreak;
            xd["alive"] = xvfCache->isXvfAlive(3000);
            xd["poll_gen"] = XVF3800_Cache::pollGen();
            xd["heart_gen"] = XVF3800_Cache::heartGen();
            xd["loop_gen"] = TelemetryBuilder::loopGen();
        }
    } else {
        doaObj["confidence"] = doaConf;
        doaObj["speech_detected"] = (doaConf >= 0.5f);
    }

}

// ---------------------------------------------------------------------------
//  build — rtsp-mic.telemetry.v1
// ---------------------------------------------------------------------------

char *TelemetryBuilder::buildAlloc(bool includeExtended, size_t *outLen,
                                   AllocFn alloc) {
    AudioTelemetry telem;
    if (_producer) {
        _producer->getTelemetry(telem);
    } else {
        memset(&telem, 0, sizeof(telem));
    }

    DynamicJsonDocument doc(includeExtended ? 6144 : 4096);

    doc["schema"]           = "rtsp-mic.telemetry.v1";
    doc["node_id"]          = _nodeId;
    doc["timestamp_ms"]     = (_ntp && _ntp->isSynced())
                                  ? _ntp->getEpochMillis()
                                  : (int64_t)millis();
    doc["device_type"]      = NODE_TYPE_STR;
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["must_change_password"] = WebCredentials::isDefaultPassword();

    JsonObject timeObj = doc["time"].to<JsonObject>();
    writeTimeObject(timeObj, _ntp);

    int doaDeg = 0;
    float doaConf = 0.0f;
    float effectiveGainDb = 0.0f;
    if (_xvfCache) {
        XVF3800_CacheData cacheData;
        _xvfCache->getData(cacheData);
        doaDeg  = (int)cacheData.doaAzimuth;
        doaConf = xvfMaxSpEnergy(cacheData);
        DspLevelCompParams gp{};
        gp.agcGainDb = cacheData.dspGainDb;
        gp.agcEnabled = s_dspAgcEnabled.load();
        gp.micGainLin = s_dspMicGain.load();
        gp.asroutGainLin = s_asroutGain.load();
        gp.asroutEnabled = s_asroutEnabled.load() != 0;
        gp.attnsMode = s_attnsMode.load();
        gp.attnsNominal = s_attnsNominal.load();
        gp.attnsSlope = s_attnsSlope.load();
        gp.speechActive = xvfSpeechDetected(cacheData) || cacheData.vadActive;
        gp.agcGainInitLin = DSP_AGC_GAIN_INIT_LIN;
        gp.softwareGainLin = _producer ? _producer->getGain() : 1.0f;
        effectiveGainDb = dspEffectiveGainDb(gp);
    }
    JsonObject audio = doc["audio"].to<JsonObject>();
    writeAudioLevels(audio, telem, effectiveGainDb);
    writeSPL(audio, telem, _producer, effectiveGainDb);
    audio["doa_deg"] = doaDeg;
    audio["dsp_effective_gain_db"] = effectiveGainDb;
    if (_xvfCache) {
        audio["dsp_agc_gain_db"] = _xvfCache->getDspGainDb();
    }
    if (_producer) {
        audio["hpf"]          = _producer->isHpfEnabled();
        audio["hpf_mode"]     = (int)_producer->getHpfMode();
        audio["hpf_cutoff_hz"] = _producer->getHpfCutoff();
        audio["gain"]          = _producer->getGain();
    }
    audio["sample_rate"] = I2S_SAMPLE_RATE;

    if (_xvfCache) {
        XVF3800_CacheData cd;
        _xvfCache->getData(cd);
        JsonArray spEn = audio["sp_energy"].to<JsonArray>();
        for (int i = 0; i < 4; i++) spEn.add(xvfNormSpEnergy(cd.spEnergy[i]));
    }

    JsonObject sys = doc["system"].to<JsonObject>();
    writeSystem(sys);

    if (includeExtended) {
        writeExtendedFields(doc, _producer, _xvfCache, _ntp, _rtspClient,
                           telem, doaDeg, doaConf);
    }

    if (doc.overflowed()) {
        Serial.printf("[TELEM] JSON overflow! Returning minimal telemetry.\n");
        DynamicJsonDocument minDoc(256);
        minDoc["schema"] = "rtsp-mic.telemetry.v1";
        minDoc["node_id"] = _nodeId;
        minDoc["timestamp_ms"] = (_ntp && _ntp->isSynced())
                                     ? _ntp->getEpochMillis()
                                     : (int64_t)millis();
        minDoc["overflow"] = true;
        return jsonToAlloc(minDoc, outLen, alloc);
    }

    return jsonToAlloc(doc, outLen, alloc);
}

String TelemetryBuilder::build(bool includeExtended) {
    size_t len = 0;
    return adoptMallocString(buildAlloc(includeExtended, &len, nullptr));
}

char *TelemetryBuilder::buildLocatorAlloc(size_t *outLen, AllocFn alloc) {
    AudioTelemetry telem;
    if (_producer) {
        _producer->getTelemetry(telem);
    } else {
        memset(&telem, 0, sizeof(telem));
    }

    DynamicJsonDocument doc(768);
    doc["schema"] = "rtsp-mic.public.v1";
    doc["node_id"] = _nodeId;
    doc["timestamp_ms"] = (_ntp && _ntp->isSynced())
                              ? _ntp->getEpochMillis()
                              : (int64_t)millis();
    doc["device_type"] = NODE_TYPE_STR;
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["must_change_password"] = WebCredentials::isDefaultPassword();

    JsonObject audio = doc["audio"].to<JsonObject>();
    audio["rms"] = telem.rms;
    audio["peak"] = telem.peak;
    audio["clipping"] = telem.clipping;
    audio["avg_db"] = telem.avgDb;

    JsonObject sys = doc["system"].to<JsonObject>();
    sys["uptime_s"] = (uint32_t)(millis() / 1000UL);
    sys["wifi_rssi"] = WiFi.RSSI();

    return jsonToAlloc(doc, outLen, alloc);
}

String TelemetryBuilder::buildLocator() {
    size_t len = 0;
    return adoptMallocString(buildLocatorAlloc(&len, nullptr));
}

// ---------------------------------------------------------------------------
//  buildEvent — rtsp-mic.event.v1
// ---------------------------------------------------------------------------

char *TelemetryBuilder::buildEventAlloc(const char *eventType, const char *details,
                                        size_t *outLen, AllocFn alloc) {
    const int64_t captureTs = (_ntp && _ntp->isSynced())
                                  ? _ntp->getEpochMillis()
                                  : (int64_t)millis();
    const int64_t publishTs = captureTs;

    DynamicJsonDocument doc(512);
    doc["schema"]               = "rtsp-mic.event.v1";
    doc["node_id"]              = _nodeId;
    doc["capture_timestamp_ms"] = captureTs;
    doc["publish_timestamp_ms"] = publishTs;
    doc["device_type"]          = NODE_TYPE_STR;
    doc["event_type"]           = eventType ? eventType : "";
    doc["details"]              = details ? details : "";
    return jsonToAlloc(doc, outLen, alloc);
}

String TelemetryBuilder::buildEvent(const char *eventType, const char *details) {
    size_t len = 0;
    return adoptMallocString(buildEventAlloc(eventType, details, &len, nullptr));
}

// ---------------------------------------------------------------------------
//  setCalibrationOffsetDb
// ---------------------------------------------------------------------------

void TelemetryBuilder::setCalibrationOffsetDb(float offsetDb) {
    s_calibrationOffsetDb.store(offsetDb);
}

void TelemetryBuilder::setDspAgcState(bool enabled) {
    s_dspAgcEnabled.store(enabled);
}

void TelemetryBuilder::setDspLimiterState(bool enabled) {
    s_dspLimiterEnabled.store(enabled);
}

void TelemetryBuilder::setAecEnvState(uint8_t mode) {
    s_aecEnvMode.store(mode);
}

void TelemetryBuilder::setEchoSuppressionState(bool enabled) {
    s_echoSuppression.store(enabled);
}

void TelemetryBuilder::setAsroutState(uint8_t enabled) {
    s_asroutEnabled.store(enabled ? 1 : 0);
}

void TelemetryBuilder::setLoudspeakerPresentState(bool present) {
    s_loudspeakerPresent.store(present);
}

void TelemetryBuilder::setDspMicGainState(float gain) {
    s_dspMicGain.store(gain);
}

void TelemetryBuilder::setDspPathGains(float micGain, float asroutGain, uint8_t asroutEnabled,
                                       uint8_t attnsMode, float attnsNominal, float attnsSlope) {
    s_dspMicGain.store(micGain);
    s_asroutGain.store(asroutGain);
    s_asroutEnabled.store(asroutEnabled ? 1 : 0);
    s_attnsMode.store(attnsMode ? 1 : 0);
    s_attnsNominal.store(attnsNominal);
    s_attnsSlope.store(attnsSlope);
}

void TelemetryBuilder::setSecurityControls(uint8_t ledMode) {
    s_ledMode.store(ledMode);
}

uint8_t TelemetryBuilder::ledMode() {
    return s_ledMode.load();
}

// ---------------------------------------------------------------------------
//  buildHeartbeat — rtsp-mic.status.v1
// ---------------------------------------------------------------------------

char *TelemetryBuilder::buildHeartbeatAlloc(const char *status, size_t *outLen,
                                            AllocFn alloc) {
    DynamicJsonDocument doc(384);
    doc["schema"]       = "rtsp-mic.status.v1";
    doc["node_id"]      = _nodeId;
    doc["timestamp_ms"] = (_ntp && _ntp->isSynced())
                              ? _ntp->getEpochMillis()
                              : (int64_t)millis();
    doc["device_type"]  = NODE_TYPE_STR;
    doc["status"]       = status ? status : "unknown";

    JsonObject sys = doc["system"].to<JsonObject>();
    sys["temp_c"]    = temperatureRead();
    sys["cpu_mhz"]   = (int)(ESP.getCpuFreqMHz());
    sys["wifi_rssi"] = (int)WiFi.RSSI();
    sys["free_heap"] = ESP.getFreeHeap();
    sys["uptime_s"]  = millis() / 1000;

    return jsonToAlloc(doc, outLen, alloc);
}

String TelemetryBuilder::buildHeartbeat(const char *status) {
    size_t len = 0;
    return adoptMallocString(buildHeartbeatAlloc(status, &len, nullptr));
}
