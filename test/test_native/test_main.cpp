/**
 * @file test_main.cpp
 * @brief Native unit tests for RTSP Mic (bird / nature recording).
 */

#include <unity.h>
#include <ArduinoJson.h>
#include <math.h>
#include <cstring>
#include <atomic>
#include <thread>

#include "HighPassFilter.h"
#include "AudioConvert.h"
#include "XVF3800_I2C.h"
#include "XVF3800_Cache.h"
#include "TelemetryBuilder.h"
#include "MelSpectrogram.h"
#include "NTPClient.h"
#include "RTSPClient.h"
#include "SoundLevelMeter.h"
#include "AudioProducer.h"
#include "SystemMonitor.h"
#include "LedIndicator.h"
#include "LivenessWatchdog.h"
#include "test_mocks.h"
#include "Config.h"
#include "EncodedAudioFanout.h"
#include "AudioLifecycle.h"
#include "PcmFrameAccumulator.h"
#include "TaskLifecycle.h"
#include "FreeRtosTaskHandshake.h"
#include "RtpStreamGuard.h"
#include "RtspRuntimeCore.h"
#include "CommandAuth.h"
#include "WsTicketAuth.h"
#include "ValidateUtil.h"
#include "NtpSyncState.h"
#include "WiFiRecovery.h"
#include "NetConfig.h"
#include "DspLevelCompensate.h"
#include "MQTTManager.h"
#include "WebCredentials.h"
#include "WsTelemetryGate.h"

// Forward declarations: Mel atmospheric attenuation helpers
// Forward declarations: Sensor intelligence Phase 1
void test_dsp_level_compensate_agc_off_ignores_chip_gain(void);
void test_ws_telemetry_gate_backpressure(void);

// Extended coverage (test_extended_coverage.cpp)
// Forward declaration for NVS write fail flag
extern bool g_prefs_write_fail;

namespace RtspMicTest {
extern bool     g_vadActive;
extern uint16_t g_doaAzimuth;
extern float    g_speechDetected;
extern float    g_spEnergy[4];
extern XVF3800_Result g_vadResult;
extern XVF3800_Result g_doaResult;
extern int64_t  g_epochMillis;
extern bool     g_ntpSynced;
extern bool     g_rtspStreaming;
}

extern void test_millis_set(uint32_t value);
extern void test_millis_advance(uint32_t delta);

// ---------------------------------------------------------------------------
// HighPassFilter
// ---------------------------------------------------------------------------

void test_hpf_disabled_passthrough() {
    HighPassFilter hpf(300.0f, 16000.0f);
    hpf.setEnabled(false);
    TEST_ASSERT_EQUAL_INT16(1000, hpf.process(1000));
    TEST_ASSERT_EQUAL_INT16(-2000, hpf.process(-2000));
}

void test_hpf_attenuates_dc() {
    HighPassFilter hpf(300.0f, 16000.0f);
    hpf.setEnabled(true);

    float sum = 0.0f;
    for (int i = 0; i < 8000; i++) {
        sum += fabsf(static_cast<float>(hpf.process(10000)));
    }
    float avg = sum / 8000.0f;
    TEST_ASSERT_LESS_THAN(500.0f, avg);
}

void test_hpf_process_block() {
    HighPassFilter hpf(300.0f, 16000.0f);
    int16_t buf[4] = {1000, 1000, 1000, 1000};
    hpf.processBlock(buf, 4);
    TEST_ASSERT_NOT_EQUAL(1000, buf[3]);
}

void test_hpf_set_cutoff() {
    HighPassFilter hpf(120.0f, 16000.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 120.0f, hpf.getCutoff());
    hpf.setCutoff(80.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 80.0f, hpf.getCutoff());
    hpf.setCutoff(5.0f);  /* clamp to 20 */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f, hpf.getCutoff());
    hpf.setCutoff(9000.0f); /* clamp to 0.45*fs */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 7200.0f, hpf.getCutoff());
}

// ---------------------------------------------------------------------------
// AudioConvert
// ---------------------------------------------------------------------------

void test_audio_convert_stereo_to_mono() {
    int32_t i2s[4] = {
        static_cast<int32_t>(1000) << 16,
        static_cast<int32_t>(3000) << 16,
        static_cast<int32_t>(-2000) << 16,
        static_cast<int32_t>(2000) << 16,
    };
    int16_t pcm[4] = {0};
    size_t n = audioConvertI2SToMono(i2s, 4, pcm, 65536, nullptr, nullptr);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_INT16(1000, pcm[0]);
    TEST_ASSERT_EQUAL_INT16(-2000, pcm[1]);
}

void test_audio_convert_gain_clipping() {
    int32_t i2s[2] = {
        static_cast<int32_t>(30000) << 16,
        static_cast<int32_t>(30000) << 16,
    };
    int16_t pcm[1] = {0};
    audioConvertI2SToMono(i2s, 2, pcm, 655360, nullptr, nullptr);
    TEST_ASSERT_EQUAL_INT16(PCM_MAX_AMP, pcm[0]);
}

// ---------------------------------------------------------------------------
// XVF3800_Cache
// ---------------------------------------------------------------------------

void test_xvf_cache_reads_vad_doa() {
    RtspMicTest::g_vadActive     = true;
    RtspMicTest::g_doaAzimuth    = 90;
    RtspMicTest::g_doaResult     = XVF3800_Result::OK;
    test_millis_set(0);

    XVF3800_I2C xvf;
    xvf.begin();
    XVF3800_Cache cache(&xvf);
    test_millis_advance(XVF_POLL_INTERVAL_MS);
    cache.update();

    XVF3800_CacheData data;
    cache.getData(data);
    TEST_ASSERT_TRUE(data.vadActive);
    TEST_ASSERT_EQUAL_UINT16(90, data.doaAzimuth);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, data.speechDetected);
}

void test_xvf_cache_skips_failed_i2c() {
    RtspMicTest::g_vadActive  = true;
    RtspMicTest::g_doaResult  = XVF3800_Result::ERR_TIMEOUT;
    test_millis_set(0);

    XVF3800_I2C xvf;
    xvf.begin();
    XVF3800_Cache cache(&xvf);
    cache.update();

    TEST_ASSERT_FALSE(cache.isVADActive());
    RtspMicTest::g_doaResult = XVF3800_Result::OK;
}

// ---------------------------------------------------------------------------
// TelemetryBuilder
// ---------------------------------------------------------------------------

void test_telemetry_v2_standard_fields() {
    RtspMicTest::g_vadActive     = false;
    RtspMicTest::g_doaAzimuth    = 180;
    RtspMicTest::g_speechDetected = 0.85f;
    RtspMicTest::g_ntpSynced     = true;
    RtspMicTest::g_epochMillis   = 1704067200000LL;

    XVF3800_I2C xvf;
    xvf.begin();
    XVF3800_Cache cache(&xvf);
    test_millis_advance(XVF_POLL_INTERVAL_MS);
    cache.update();

    NTPClient ntp;
    RTSPClient rtsp;

    TelemetryBuilder::init(nullptr, &cache, &ntp, &rtsp, "A1B2C3");
    String json = TelemetryBuilder::build(false);

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, json.c_str());
    TEST_ASSERT_TRUE(err == DeserializationError::Ok);
    // v2 schema
    TEST_ASSERT_EQUAL_STRING("rtsp-mic.telemetry.v1", doc["schema"]);
    TEST_ASSERT_EQUAL_STRING("A1B2C3", doc["node_id"]);
    TEST_ASSERT_EQUAL_INT64(1704067200000LL, doc["timestamp_ms"].as<int64_t>());
    // time object
    TEST_ASSERT_TRUE(doc.containsKey("time"));
    TEST_ASSERT_TRUE(doc["time"].containsKey("synced"));
    TEST_ASSERT_TRUE(doc["time"].containsKey("age_ms"));
    TEST_ASSERT_TRUE(doc["time"].containsKey("uncertainty_ms"));
    // calibration_offset_db
    TEST_ASSERT_TRUE(doc["audio"].containsKey("calibration_offset_db"));
    // system
    TEST_ASSERT_TRUE(doc.containsKey("system"));
    TEST_ASSERT_TRUE(doc["system"].containsKey("temp_c"));
    TEST_ASSERT_TRUE(doc["system"].containsKey("wifi_rssi"));
    TEST_ASSERT_TRUE(doc["system"].containsKey("uptime_s"));
#if !defined(FIRMWARE_DEV)
#endif
    // not in non-extended
    TEST_ASSERT_FALSE(doc.containsKey("ntp_synced"));
    TEST_ASSERT_FALSE(doc.containsKey("rtsp_streaming"));
    TEST_ASSERT_EQUAL_INT(180, doc["audio"]["doa_deg"]);
    TEST_ASSERT_TRUE(doc["audio"].containsKey("peak_level"));
    TEST_ASSERT_TRUE(doc["audio"].containsKey("rms_db"));
#if !defined(FIRMWARE_DEV)
#endif
}

void test_telemetry_v2_doa_uses_chip_azimuth() {
    RtspMicTest::g_doaAzimuth = 90;
    RtspMicTest::g_speechDetected = 0.7f;
    RtspMicTest::g_ntpSynced = true;
    RtspMicTest::g_epochMillis = 1704067200000LL;

    XVF3800_I2C xvf;
    xvf.begin();
    XVF3800_Cache cache(&xvf);
    test_millis_advance(XVF_POLL_INTERVAL_MS);
    cache.update();

    NTPClient ntp;
    RTSPClient rtsp;
    TelemetryBuilder::init(nullptr, &cache, &ntp, &rtsp, "A1B2C3");

    // Weather/OWM wind DoA removed: telemetry always uses chip azimuth.
    String json = TelemetryBuilder::build(false);
    DynamicJsonDocument doc(8192);
    TEST_ASSERT_TRUE(deserializeJson(doc, json.c_str()) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_INT(90, doc["audio"]["doa_deg"].as<int>());
}

void test_telemetry_v2_extended_webui_fields() {
    RtspMicTest::g_rtspStreaming = true;
    RtspMicTest::g_vadActive     = true;

    XVF3800_I2C xvf;
    xvf.begin();
    XVF3800_Cache cache(&xvf);
    test_millis_advance(XVF_POLL_INTERVAL_MS);
    cache.update();

    NTPClient ntp;
    RTSPClient rtsp;

    TelemetryBuilder::init(nullptr, &cache, &ntp, &rtsp, "DEADBE");
    // rtsp_streaming = локальные RTSP-клиенты, не remote RTSPClient.
    TelemetryBuilder::setLocalRtspClientCount(1);
    String json = TelemetryBuilder::build(true);

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, json.c_str());
    TEST_ASSERT_TRUE(err == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("rtsp-mic.telemetry.v1", doc["schema"]);
    TEST_ASSERT_TRUE(doc["rtsp_streaming"]);
    TEST_ASSERT_TRUE(doc.containsKey("doa"));
    TEST_ASSERT_EQUAL_STRING("test-ssid", doc["wifi_ssid"]);
    TelemetryBuilder::setLocalRtspClientCount(0);
}


void test_mel_copy_recent_frames_chrono() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());
    float frame[MelSpectrogram::kNumBands];
    for (int f = 0; f < 10; f++) {
        for (int b = 0; b < MelSpectrogram::kNumBands; b++) {
            frame[b] = (float)(f * 100 + b);
        }
        mel.pushFrame(frame);
    }
    float dst[5 * MelSpectrogram::kNumBands];
    TEST_ASSERT_TRUE(mel.copyRecentFrames(dst, 5));
    // Chronological: frames 5..9
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 500.0f, dst[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 501.0f, dst[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 900.0f, dst[4 * MelSpectrogram::kNumBands]);
    TEST_ASSERT_FALSE(mel.copyRecentFrames(dst, 11)); /* not enough */
}


void test_telemetry_no_silent_truncation() {
    RtspMicTest::g_ntpSynced   = true;
    RtspMicTest::g_epochMillis = 1704067200000LL;
    NTPClient ntp;
    TelemetryBuilder::init(nullptr, nullptr, &ntp, nullptr, "A1B2C3");

    // Standard build — must produce valid parseable JSON
    String json = TelemetryBuilder::build(false);
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, json.c_str());
    TEST_ASSERT_TRUE_MESSAGE(err == DeserializationError::Ok, json.c_str());
    // measureJson must not truncate
    TEST_ASSERT_TRUE(json.length() > 50);  // non-trivial

    // Alloc path: one buffer, no intermediate String
    size_t allocLen = 0;
    char *allocBuf = TelemetryBuilder::buildAlloc(false, &allocLen, nullptr);
    TEST_ASSERT_NOT_NULL(allocBuf);
    TEST_ASSERT_TRUE(allocLen > 50);
    TEST_ASSERT_EQUAL(allocLen, strlen(allocBuf));
    DynamicJsonDocument allocDoc(4096);
    TEST_ASSERT_TRUE(deserializeJson(allocDoc, allocBuf) == DeserializationError::Ok);
    free(allocBuf);

    // Event build
    String evJson = TelemetryBuilder::buildEvent("test_event", "test_detail");
    DynamicJsonDocument evDoc(512);
    err = deserializeJson(evDoc, evJson.c_str());
    TEST_ASSERT_TRUE(err == DeserializationError::Ok);

    // Heartbeat / status
    String hbJson = TelemetryBuilder::buildHeartbeat("online");
    DynamicJsonDocument hbDoc(512);
    err = deserializeJson(hbDoc, hbJson.c_str());
    TEST_ASSERT_TRUE_MESSAGE(err == DeserializationError::Ok, hbJson.c_str());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// MelSpectrogram
// ---------------------------------------------------------------------------

void test_mel_begin_end() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());
    TEST_ASSERT_EQUAL_INT(0, mel.getFrameCount());
    TEST_ASSERT_EQUAL_INT(0, mel.getHead());
}

void test_mel_compute_frame_unity_sine() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());

    /* computeFrame ожидает kWindowLength (400) сэмплов, zero-pad до 512 */
    int16_t pcm[MelSpectrogram::kWindowLength];
    for (int i = 0; i < MelSpectrogram::kWindowLength; i++) {
        float t = (float)i / MelSpectrogram::kWindowLength;
        pcm[i] = (int16_t)(16000.0f * sinf(2.0f * M_PI * 440.0f * t / 16000.0f));
    }

    float melOut[MelSpectrogram::kNumBands];
    mel.computeFrame(pcm, melOut);

    bool hasEnergy = false;
    for (int b = 0; b < MelSpectrogram::kNumBands; b++) {
        if (!isnan(melOut[b]) && !isinf(melOut[b]) && melOut[b] > -60.0f) {
            hasEnergy = true;
        }
    }
    TEST_ASSERT_TRUE(hasEnergy);
}

void test_mel_push_get_frame_count() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());

    float frame[MelSpectrogram::kNumBands];
    memset(frame, 0, sizeof(frame));

    int16_t pcm[MelSpectrogram::kWindowLength];
    memset(pcm, 0, sizeof(pcm));
    mel.computeFrame(pcm, frame);

    mel.pushFrame(frame);
    TEST_ASSERT_EQUAL_INT(1, mel.getFrameCount());
    TEST_ASSERT_EQUAL_INT(1, mel.getHead());  /* head = next write pos */
}

void test_mel_push_multiple_frames() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());

    float frame[MelSpectrogram::kNumBands];
    memset(frame, 0, sizeof(frame));

    int16_t pcm[MelSpectrogram::kWindowLength];
    memset(pcm, 0, sizeof(pcm));

    for (int i = 0; i < MelSpectrogram::kNumFrames + 50; i++) {
        frame[0] = (float)i;
        mel.computeFrame(pcm, frame);
        mel.pushFrame(frame);
    }

    TEST_ASSERT_EQUAL_INT(MelSpectrogram::kNumFrames, mel.getFrameCount());
    TEST_ASSERT_TRUE(mel.getHead() < MelSpectrogram::kNumFrames);
}

void test_mel_get_spectrogram_buffer() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());

    const float *spec = mel.getSpectrogram();
    TEST_ASSERT_NOT_NULL(spec);
}

void test_mel_shutdown_rejects_new_writer_work() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());

    float frame[MelSpectrogram::kNumBands] = {};
    mel.pushFrame(frame);
    TEST_ASSERT_EQUAL_INT(1, mel.getFrameCount());

    mel.requestShutdown();
    mel.pushFrame(frame);
    TEST_ASSERT_EQUAL_INT(1, mel.getFrameCount());
}

void test_mel_reset_for_restart_reenables_writers() {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());

    float frame[MelSpectrogram::kNumBands] = {};
    mel.pushFrame(frame);
    mel.requestShutdown();
    TEST_ASSERT_TRUE(mel.resetForRestart());
    TEST_ASSERT_EQUAL_INT(0, mel.getFrameCount());

    mel.pushFrame(frame);
    TEST_ASSERT_EQUAL_INT(1, mel.getFrameCount());
}

// ---------------------------------------------------------------------------
// SystemMonitor — thermal protection + ring buffer health
// ---------------------------------------------------------------------------

void test_thermal_initial_state() {
    mock_audio_reset();
    mock_temperature_set(25.0f);
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    TEST_ASSERT_FALSE(sm.isThrottled());
    TEST_ASSERT_FALSE(sm.isShutdown());
    sm.test_checkThermal();
    TEST_ASSERT_EQUAL_FLOAT(25.0f, sm.getTemperature());
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RecoveryLevel::NONE),
                      static_cast<uint8_t>(sm.getLastRecovery()));
}

void test_thermal_throttle_at_85c() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    mock_temperature_set(85.0f);
    sm.test_checkThermal();

    TEST_ASSERT_TRUE(sm.isThrottled());
    TEST_ASSERT_FALSE(sm.isShutdown());
    TEST_ASSERT_EQUAL(160, mock_cpu_freq_get());
}

void test_thermal_no_throttle_at_75c() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    mock_temperature_set(75.0f);
    sm.test_checkThermal();

    TEST_ASSERT_FALSE(sm.isThrottled());
    TEST_ASSERT_EQUAL(240, mock_cpu_freq_get());
}

void test_thermal_shutdown_at_95c() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    mock_temperature_set(95.0f);
    sm.test_checkThermal();

    TEST_ASSERT_TRUE(sm.isShutdown());
    // Shutdown-путь делает return до throttle-блока → _throttled не выставляется
    TEST_ASSERT_FALSE(sm.isThrottled());
    TEST_ASSERT_EQUAL(80, mock_cpu_freq_get());
    TEST_ASSERT_EQUAL(1, mock_audio_get_stop_calls());
    TEST_ASSERT_EQUAL(0, mock_audio_get_begin_calls());
}

void test_thermal_cooldown_recovery() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    // Вызываем shutdown при 95°C
    mock_temperature_set(95.0f);
    sm.test_checkThermal();
    TEST_ASSERT_TRUE(sm.isShutdown());
    TEST_ASSERT_EQUAL(1, mock_audio_get_stop_calls());

    // Остыло до 65°C — восстановление
    mock_temperature_set(65.0f);
    sm.test_checkThermal();

    TEST_ASSERT_FALSE(sm.isShutdown());
    TEST_ASSERT_FALSE(sm.isThrottled());
    TEST_ASSERT_EQUAL(240, mock_cpu_freq_get());
    TEST_ASSERT_EQUAL(1, mock_audio_get_begin_calls());
}

void test_thermal_no_shutdown_wait_still_hot() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    // Shutdown
    mock_temperature_set(95.0f);
    sm.test_checkThermal();
    TEST_ASSERT_TRUE(sm.isShutdown());

    // 80°C — ещё не остыло до 70
    mock_temperature_set(80.0f);
    sm.test_checkThermal();
    TEST_ASSERT_TRUE(sm.isShutdown());  // всё ещё shutdown
    TEST_ASSERT_EQUAL(1, mock_audio_get_stop_calls());  // повторно не вызывался
    TEST_ASSERT_EQUAL(0, mock_audio_get_begin_calls()); // begin не вызывался
}

void test_thermal_persistent_latch() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    // Throttle при 85°C
    mock_temperature_set(85.0f);
    sm.test_checkThermal();
    TEST_ASSERT_TRUE(sm.isThrottled());

    // Temp 75°C — но latch активен, throttle не снимается
    mock_temperature_set(75.0f);
    sm.test_checkThermal();
    TEST_ASSERT_TRUE(sm.isThrottled()); // latch держит

    // Только при <70°C latch снимается
    mock_temperature_set(65.0f);
    sm.test_checkThermal();
    TEST_ASSERT_FALSE(sm.isThrottled());
    TEST_ASSERT_EQUAL(240, mock_cpu_freq_get());
}

void test_ringbuf_health_no_overflow() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    // Нормальная утилизация, нет капель
    mock_audio_set_available(1024);        // ~3% утилизации
    mock_audio_set_ringbuf(reinterpret_cast<void *>(1));

    AudioTelemetry telem = {};
    telem.ringBufferDrops = 0;
    mock_audio_set_telemetry(telem);

    sm.test_checkRingBufferHealth();
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RecoveryLevel::NONE),
                      static_cast<uint8_t>(sm.getLastRecovery()));
}

void test_ringbuf_health_overflow_triggers_recovery() {
    mock_audio_reset();
    mock_cpu_freq_reset();
    AudioProducer dummy;
    SystemMonitor sm(&dummy);

    mock_audio_set_available(32000);
    mock_audio_set_ringbuf(reinterpret_cast<void *>(1));

    // 5 последовательных кадров с новыми каплями
    AudioTelemetry telem = {};
    for (int i = 0; i < 5; i++) {
        telem.ringBufferDrops = i + 1;
        mock_audio_set_telemetry(telem);
        sm.test_checkRingBufferHealth();
    }
    // Recovery НЕ сработал из-за cooldown (millis=0 + setUp reset)
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RecoveryLevel::NONE),
                      static_cast<uint8_t>(sm.getLastRecovery()));

    // Продвигаем millis на 65000 мс — cooldown истёк
    test_millis_advance(65000);

    // Ещё 5 кадров с каплями — теперь recovery должен сработать
    for (int i = 0; i < 5; i++) {
        telem.ringBufferDrops = 10 + i + 1;
        mock_audio_set_telemetry(telem);
        sm.test_checkRingBufferHealth();
    }
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RecoveryLevel::I2S_RESET),
                      static_cast<uint8_t>(sm.getLastRecovery()));
    // Первый recovery подавлен cooldown → только 1 I2S reset
    TEST_ASSERT_EQUAL(1, mock_audio_get_stop_calls());
    TEST_ASSERT_EQUAL(1, mock_audio_get_begin_calls());

    // Восстановление состояния millis для следующих тестов
    test_millis_set(0);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------


void test_ws_telemetry_gate_backpressure() {
    auto ok = wsTelemetryDecide(true, true, true, false, 40000, 100);
    TEST_ASSERT_TRUE(ok.send);

    auto bp = wsTelemetryDecide(true, true, false, false, 40000, 100);
    TEST_ASSERT_FALSE(bp.send);
    TEST_ASSERT_TRUE(bp.skipBackpressure);

    auto full = wsTelemetryDecide(true, true, true, true, 40000, 100);
    TEST_ASSERT_FALSE(full.send);
    TEST_ASSERT_TRUE(full.skipBackpressure);

    auto heap = wsTelemetryDecide(true, true, true, false, 1000, 100);
    TEST_ASSERT_FALSE(heap.send);
    TEST_ASSERT_TRUE(heap.skipLowHeap);

    auto unauth = wsTelemetryDecide(true, false, true, false, 40000, 100);
    TEST_ASSERT_FALSE(unauth.send);
    TEST_ASSERT_TRUE(unauth.skipUnauthed);

    auto oversize = wsTelemetryDecide(true, true, true, false, 40000, WS_TELEM_MAX_PAYLOAD + 1);
    TEST_ASSERT_FALSE(oversize.send);
    TEST_ASSERT_TRUE(oversize.skipOversize);
}


// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SoundLevelMeter — LAeq + A-weighting

static void fillSine(int16_t *buf, int n, float freqHz, float fs, float amplitude) {
    for (int i = 0; i < n; i++) {
        float tt = static_cast<float>(i) / fs;
        float val = amplitude * sinf(2.0f * (float)M_PI * freqHz * tt);
        buf[i] = static_cast<int16_t>(val * 32767.0f);
    }
}


// ---------------------------------------------------------------------------

void test_slm_initial_state() {
    SoundLevelMeter slm;
    TEST_ASSERT_TRUE_MESSAGE(slm.isEnabled(), "Enabled by default");
    TEST_ASSERT_EQUAL(0, slm.getSamplesProcessed());
    TEST_ASSERT_MESSAGE(slm.getLAeq() < -150.0f, "LAeq silence: must be very low");
}

void test_slm_silence_stays_low() {
    SoundLevelMeter slm;
    int16_t silence[512] = {};
    for (int i = 0; i < 10; i++) slm.processFrame(silence, 512);

    TEST_ASSERT_EQUAL(512 * 10, slm.getSamplesProcessed());
    TEST_ASSERT_MESSAGE(slm.getLAeq() < -110.0f, "Silence LAeq: must be very low");
    TEST_ASSERT_MESSAGE(slm.getFastSPL() < -100.0f, "Silence FAST SPL");
    TEST_ASSERT_MESSAGE(slm.getSlowSPL() < -100.0f, "Silence SLOW SPL");
}

void test_slm_loud_signal() {
    SoundLevelMeter slm;
    int16_t loud[512];
    fillSine(loud, 512, 1000.0f, 16000.0f, 1.0f);

    for (int i = 0; i < 10; i++) slm.processFrame(loud, 512);
    float laeq = slm.getLAeq();
    // Синус 1кГц на полной амплитуде должен давать LAeq около -3 дБ на незакалиброванном P0
    TEST_ASSERT_MESSAGE(laeq > -12.0f, "Loud 1kHz: expected > -12 dB");
}

void test_slm_a_weighting_reduces_low_freq() {
    SoundLevelMeter slmLow, slmMid;
    int16_t low[512], mid[512];
    fillSine(low, 512, 50.0f, 16000.0f, 1.0f);
    fillSine(mid, 512, 1000.0f, 16000.0f, 1.0f);

    for (int i = 0; i < 5; i++) slmLow.processFrame(low, 512);
    for (int i = 0; i < 5; i++) slmMid.processFrame(mid, 512);

    float laeqLow = slmLow.getLAeq();
    float laeqMid = slmMid.getLAeq();
    // A-weighting: НЧ должны быть ниже, но точная дельта зависит от фильтра
    // (упрощённые SOS-коэффициенты для Fs=16k могут дать меньше затухания)
    TEST_ASSERT_MESSAGE(laeqLow < laeqMid,
        "A-weighting must attenuate 50 Hz relative to 1 kHz");
}

void test_slm_fast_vs_slow() {
    SoundLevelMeter slm;
    int16_t burst[512];
    fillSine(burst, 512, 1000.0f, 16000.0f, 1.0f);

    // Несколько кадров, чтобы FAST набрал энергию быстрее SLOW
    for (int i = 0; i < 3; i++) slm.processFrame(burst, 512);
    float fast = slm.getFastSPL();
    float slow = slm.getSlowSPL();

    TEST_ASSERT_MESSAGE(fast > slow, "FAST (125ms) must have higher SPL than SLOW (1000ms) after burst");
}

void test_slm_reset_la_eq() {
    SoundLevelMeter slm;
    int16_t loud[512];
    fillSine(loud, 512, 1000.0f, 16000.0f, 1.0f);

    for (int i = 0; i < 5; i++) slm.processFrame(loud, 512);
    float laeq1 = slm.getLAeq();
    TEST_ASSERT_MESSAGE(laeq1 > -10.0f, "Before reset: LAeq must be > -10 dB");

    slm.resetLAeq();
    float laeq2 = slm.getLAeq();
    TEST_ASSERT_MESSAGE(laeq2 < laeq1 - 20.0f, "LAeq after reset must be much lower");
    TEST_ASSERT_EQUAL(0, slm.getSamplesProcessed());
}

void test_slm_disabled_skips() {
    SoundLevelMeter slm;
    slm.setEnabled(false);
    TEST_ASSERT_FALSE_MESSAGE(slm.isEnabled(), "Disabled");

    int16_t loud[512];
    fillSine(loud, 512, 1000.0f, 16000.0f, 1.0f);

    for (int i = 0; i < 10; i++) slm.processFrame(loud, 512);
    TEST_ASSERT_EQUAL(0, slm.getSamplesProcessed());
    TEST_ASSERT_MESSAGE(slm.getLAeq() < -150.0f, "Disabled: LAeq stays at noise floor");
}

// ---------------------------------------------------------------------------
// LedIndicator — драйвер LED-индикации
// ---------------------------------------------------------------------------

void test_led_indicator_initial_state() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    TEST_ASSERT_TRUE_MESSAGE(led.isEnabled(), "enabled by default");
    TEST_ASSERT_EQUAL(LedIndicator::OFF, led.getPattern());
    // После begin LED должен быть LOW
    int st = mock_gpio_get_state(PIN_LED_STATUS);
    TEST_ASSERT_EQUAL(0, st);
}

void test_led_indicator_static_on() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::STATIC_ON);
    TEST_ASSERT_EQUAL(LedIndicator::STATIC_ON, led.getPattern());
    TEST_ASSERT_EQUAL(1, mock_gpio_get_state(PIN_LED_STATUS));
}

void test_led_indicator_static_off() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::STATIC_OFF);
    TEST_ASSERT_EQUAL(0, mock_gpio_get_state(PIN_LED_STATUS));
}

void test_led_indicator_blink_startup() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::BLINK_STARTUP);

    // Первый update — LED зажигается
    led.update();
    TEST_ASSERT_EQUAL(1, mock_gpio_get_state(PIN_LED_STATUS));

    // Через 200ms — гаснет
    test_millis_advance(210);
    led.update();
    TEST_ASSERT_EQUAL(0, mock_gpio_get_state(PIN_LED_STATUS));

    // Ещё через 200ms — остаётся выключен (однократный)
    test_millis_advance(210);
    led.update();
    TEST_ASSERT_EQUAL(0, mock_gpio_get_state(PIN_LED_STATUS));
}

void test_led_indicator_blink_error() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::BLINK_ERROR);

    // 3× 100ms on/off = 6 переключений, потом OFF
    for (int i = 0; i < 6; i++) {
        test_millis_advance(110);
        led.update();
    }
    // После 6 фаз — LED выключен
    TEST_ASSERT_EQUAL(0, mock_gpio_get_state(PIN_LED_STATUS));
}

void test_led_indicator_blink_net_fail() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::BLINK_NET_FAIL);

    // Первый update — LED зажигается
    led.update();
    TEST_ASSERT_EQUAL(1, mock_gpio_get_state(PIN_LED_STATUS));

    // Через 500ms — гаснет
    test_millis_advance(510);
    led.update();
    TEST_ASSERT_EQUAL(0, mock_gpio_get_state(PIN_LED_STATUS));

    // Ещё через 500ms — зажигается
    test_millis_advance(510);
    led.update();
    TEST_ASSERT_EQUAL(1, mock_gpio_get_state(PIN_LED_STATUS));
}

void test_led_indicator_blink_warning() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::BLINK_WARNING);

    // 2× 200ms on/off = 4 фазы
    for (int i = 0; i < 4; i++) {
        test_millis_advance(210);
        led.update();
    }
    // После — выключен
    TEST_ASSERT_EQUAL(0, mock_gpio_get_state(PIN_LED_STATUS));
}

void test_led_indicator_level() {
    mock_gpio_reset();
    LedIndicator led(PIN_LED_STATUS);
    led.begin(true);
    led.setPattern(LedIndicator::LEVEL);
    led.setLevel(128);
    TEST_ASSERT_EQUAL(128, mock_gpio_get_state(PIN_LED_STATUS));

    led.setLevel(255);
    TEST_ASSERT_EQUAL(255, mock_gpio_get_state(PIN_LED_STATUS));
}

// ---------------------------------------------------------------------------
// LivenessWatchdog — мониторинг liveness аудио-пайплайна
// ---------------------------------------------------------------------------

void test_watchdog_initial_state() {
    LivenessWatchdog wd;
    TEST_ASSERT_TRUE_MESSAGE(wd.isEnabled(), "enabled by default");
    TEST_ASSERT_TRUE_MESSAGE(wd.isAlive(), "alive by default");
    TEST_ASSERT_FALSE_MESSAGE(wd.isStalled(), "not stalled by default");
    TEST_ASSERT_EQUAL(0, wd.failCount());
}

void test_watchdog_kick_keeps_alive() {
    LivenessWatchdog wd;
    wd.kick();
    test_millis_advance(5000);
    wd.update();
    TEST_ASSERT_TRUE_MESSAGE(wd.isAlive(), "alive after kick + 5s");
    TEST_ASSERT_FALSE_MESSAGE(wd.isStalled(), "not stalled");
}

void test_watchdog_timeout_stalls() {
    LivenessWatchdog wd;
    wd.kick();                        // t=0
    test_millis_advance(11000);       // t=11s > WATCHDOG_TIMEOUT_MS (10s)
    wd.update();
    TEST_ASSERT_FALSE_MESSAGE(wd.isAlive(), "not alive after timeout");
    TEST_ASSERT_TRUE_MESSAGE(wd.isStalled(), "stalled after timeout");
}

void test_watchdog_kick_resets_stall() {
    LivenessWatchdog wd;
    wd.kick();
    test_millis_advance(11000);
    wd.update();
    TEST_ASSERT_TRUE(wd.isStalled());

    wd.kick();                        // новый фрейм
    test_millis_advance(100);
    wd.update();
    TEST_ASSERT_TRUE_MESSAGE(wd.isAlive(), "alive after re-kick");
    TEST_ASSERT_FALSE_MESSAGE(wd.isStalled(), "not stalled after re-kick");
}

void test_watchdog_esp_restart_after_max_stall() {
    mock_esp_restart_reset();
    LivenessWatchdog wd;
    wd.setEnabled(true);
    wd.kick();

    // Трижды сталлим с cooldown между
    for (int i = 0; i < LivenessWatchdog::MAX_FAILED_RECOVERY; i++) {
        test_millis_advance(LivenessWatchdog::WATCHDOG_TIMEOUT_MS + 1000);
        wd.update();
        // wait for cooldown
        test_millis_advance(LivenessWatchdog::RECOVERY_COOLDOWN_MS + 1000);
        wd.update();
    }
    // После MAX_FAILED_RECOVERY эскалаций stall → esp_restart
    // (но в тесте — no-op, считаем вызов)
    TEST_ASSERT_TRUE_MESSAGE(mock_esp_restart_get_count() >= 1,
                              "esp_restart should be called after max stall escalate");
}

void test_watchdog_disabled_no_action() {
    LivenessWatchdog wd;
    wd.setEnabled(false);
    wd.kick();
    test_millis_advance(30000);
    wd.update();
    TEST_ASSERT_TRUE_MESSAGE(wd.isAlive(), "disabled: always alive");
    TEST_ASSERT_FALSE_MESSAGE(wd.isStalled(), "disabled: never stalled");
}

void test_watchdog_no_kick_never_stalled() {
    LivenessWatchdog wd;
    test_millis_advance(30000);
    wd.update();
    // Если kick() ни разу не вызывался — не считаем stalled
    TEST_ASSERT_TRUE_MESSAGE(wd.isAlive(), "no kick = alive");
    TEST_ASSERT_FALSE_MESSAGE(wd.isStalled(), "no kick = not stalled");
}

void test_audio_fanout_consumers_are_independent() {
    EncodedAudioFanout fanout(2);
    const uint8_t first[] = {1, 2, 3};
    const uint8_t second[] = {4, 5};
    TEST_ASSERT_TRUE(fanout.publish(first, sizeof(first), 20));
    TEST_ASSERT_TRUE(fanout.publish(second, sizeof(second), 40));

    EncodedAudioPacket packet{};
    TEST_ASSERT_TRUE(fanout.pop(EncodedAudioConsumer::LOCAL_RTSP, packet));
    TEST_ASSERT_EQUAL_UINT32(20, packet.timestampMs);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, packet.data, sizeof(first));

    TEST_ASSERT_TRUE(fanout.pop(EncodedAudioConsumer::REMOTE_RTSP, packet));
    TEST_ASSERT_EQUAL_UINT32(20, packet.timestampMs);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, packet.data, sizeof(first));
    TEST_ASSERT_TRUE(fanout.pop(EncodedAudioConsumer::REMOTE_RTSP, packet));
    TEST_ASSERT_EQUAL_UINT32(40, packet.timestampMs);
}

void test_audio_fanout_overflow_drops_oldest_per_consumer() {
    EncodedAudioFanout fanout(2);
    const uint8_t one[] = {1};
    const uint8_t two[] = {2};
    const uint8_t three[] = {3};
    TEST_ASSERT_TRUE(fanout.publish(one, sizeof(one), 20));
    TEST_ASSERT_TRUE(fanout.publish(two, sizeof(two), 40));
    TEST_ASSERT_TRUE(fanout.publish(three, sizeof(three), 60));

    EncodedAudioFanoutStats stats = fanout.stats(EncodedAudioConsumer::LOCAL_RTSP);
    TEST_ASSERT_EQUAL_UINT32(3, stats.enqueued);
    TEST_ASSERT_EQUAL_UINT32(1, stats.droppedOldest);

    EncodedAudioPacket packet{};
    TEST_ASSERT_TRUE(fanout.pop(EncodedAudioConsumer::LOCAL_RTSP, packet));
    TEST_ASSERT_EQUAL_UINT8(2, packet.data[0]);
    TEST_ASSERT_TRUE(fanout.pop(EncodedAudioConsumer::LOCAL_RTSP, packet));
    TEST_ASSERT_EQUAL_UINT8(3, packet.data[0]);
}

void test_pcm_accumulator_preserves_partial_chunks() {
    PcmFrameAccumulator<320, 960> accumulator;
    int16_t first[512];
    int16_t second[512];
    for (int i = 0; i < 512; ++i) {
        first[i] = static_cast<int16_t>(i);
        second[i] = static_cast<int16_t>(512 + i);
    }

    TEST_ASSERT_TRUE(accumulator.append(first, 512));
    int16_t frame[320];
    TEST_ASSERT_TRUE(accumulator.popFrame(frame));
    for (int i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(i, frame[i]);
    TEST_ASSERT_EQUAL_UINT32(192, accumulator.pending());

    TEST_ASSERT_TRUE(accumulator.append(second, 512));
    TEST_ASSERT_TRUE(accumulator.popFrame(frame));
    for (int i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(320 + i, frame[i]);
    TEST_ASSERT_TRUE(accumulator.popFrame(frame));
    for (int i = 0; i < 320; ++i) TEST_ASSERT_EQUAL_INT16(640 + i, frame[i]);
    TEST_ASSERT_EQUAL_UINT32(64, accumulator.pending());
}

void test_pcm_frame_clock_tracks_dropped_encoded_frame_time() {
    AudioFrameClock clock(16000);
    TEST_ASSERT_EQUAL_UINT32(0, clock.consumeFrame(320));
    TEST_ASSERT_EQUAL_UINT32(20, clock.consumeFrame(320));
    TEST_ASSERT_EQUAL_UINT32(40, clock.consumeFrame(320));
}

// ---------------------------------------------------------------------------
// FreeRtosTaskHandshake / TaskLifecycle
// ---------------------------------------------------------------------------

void test_task_lifecycle_exit_owner_clears_handle() {
    TaskLifecycle lifecycle;
    TEST_ASSERT_TRUE(lifecycle.prepareStart());
    void *handle = reinterpret_cast<void *>(0x1234);
    lifecycle.publishHandle(handle);
    lifecycle.markRunning();

    std::atomic<bool> workerStarted{false};
    std::thread worker([&]() {
        workerStarted.store(true);
        while (!lifecycle.stopRequested()) std::this_thread::yield();
        lifecycle.markExited();
    });
    while (!workerStarted.load()) std::this_thread::yield();
    lifecycle.requestStop();
    worker.join();

    TEST_ASSERT_EQUAL(TaskLifecycleState::EXITED, lifecycle.state());
    TEST_ASSERT_NULL(lifecycle.handle());
    TEST_ASSERT_TRUE(lifecycle.canReleaseResources());
}

void test_task_lifecycle_start_failure_is_reported() {
    TaskLifecycle lifecycle;
    TEST_ASSERT_TRUE(lifecycle.prepareStart());
    lifecycle.publishHandle(reinterpret_cast<void *>(0x1234));
    lifecycle.markStartFailed();
    lifecycle.markExited();
    TEST_ASSERT_TRUE(lifecycle.startFailed());
    TEST_ASSERT_NULL(lifecycle.handle());
    TEST_ASSERT_TRUE(lifecycle.prepareStart());
    lifecycle.publishHandle(reinterpret_cast<void *>(0x5678));
    lifecycle.markRunning();
    TEST_ASSERT_EQUAL_PTR(reinterpret_cast<void *>(0x5678), lifecycle.handle());
}

void test_freertos_task_handshake_blocks_until_real_ack() {
    FreeRtosTaskHandshake handshake;
    TEST_ASSERT_TRUE(handshake.prepareStart());
    TEST_ASSERT_FALSE(handshake.waitStartup(0));
    handshake.publishHandle(reinterpret_cast<void *>(0xCAFE));

    std::thread task([&]() {
        handshake.waitForRelease();
        handshake.signalStartup(true);
        while (!handshake.stopRequested()) std::this_thread::yield();
        handshake.signalExit();
    });

    handshake.releaseTask();
    TEST_ASSERT_TRUE(handshake.waitStartup(100));
    handshake.requestStop();
    TEST_ASSERT_TRUE(handshake.waitExit(100));
    task.join();
    TEST_ASSERT_NULL(handshake.handle());
}

void test_rtp_stream_guard_disconnects_before_consuming() {
    RtpStreamGuard guard;
    TEST_ASSERT_EQUAL(
        RtpStreamDecision::DISCONNECT,
        guard.beforeConsume(false));
    TEST_ASSERT_EQUAL(
        RtpStreamDecision::CONTINUE,
        guard.beforeConsume(true));
}

void test_rtp_stream_guard_rejects_partial_write() {
    RtpStreamGuard guard;
    TEST_ASSERT_EQUAL(
        RtpStreamDecision::DISCONNECT,
        guard.afterWrite(true, 100, 99));
    TEST_ASSERT_EQUAL(
        RtpStreamDecision::DISCONNECT,
        guard.afterWrite(false, 100, 100));
    TEST_ASSERT_EQUAL(
        RtpStreamDecision::CONTINUE,
        guard.afterWrite(true, 100, 100));
}

// ---------------------------------------------------------------------------
// ReconnectBackoff — growth, ceiling, reset after success
// ---------------------------------------------------------------------------

void test_reconnect_backoff_grows_and_resets() {
    ReconnectBackoff backoff(1000, 30000);
    backoff.recordFailure(100);
    TEST_ASSERT_FALSE(backoff.ready(2099));
    TEST_ASSERT_TRUE(backoff.ready(2100));
    backoff.recordFailure(2100);
    TEST_ASSERT_FALSE(backoff.ready(6099));
    TEST_ASSERT_TRUE(backoff.ready(6100));
    backoff.recordSuccess();
    TEST_ASSERT_EQUAL_UINT32(1000, backoff.delayMs());
}

void test_rtsp_intermediate_timeout_requires_reconnect() {
    TEST_ASSERT_EQUAL(
        RtspControlResponseAction::RECONNECT,
        RtspControlResponsePolicy::afterWait(false));
    TEST_ASSERT_EQUAL(
        RtspControlResponseAction::PROCESS_RESPONSE,
        RtspControlResponsePolicy::afterWait(true));
}

void test_rtsp_connect_backoff_uses_time_after_attempt() {
    uint32_t now = 100;
    ReconnectBackoff backoff(1000, 30000);
    bool connected = runRtspConnectAttempt(
        [&]() {
            now = 450;
            return false;
        },
        [&]() { return now; },
        backoff);

    TEST_ASSERT_FALSE(connected);
    TEST_ASSERT_FALSE(backoff.ready(2449));
    TEST_ASSERT_TRUE(backoff.ready(2450));
}

class TestInterleavedTransport {
public:
    bool connected() const { return connectedState; }
    size_t write(const uint8_t *, size_t size) {
        if (failAfterProgress && totalWritten >= failAfterProgress) return 0;
        size_t n = writeChunk < size ? writeChunk : size;
        if (writeLimitAbsolute && totalWritten + n > writeLimitAbsolute) {
            n = writeLimitAbsolute > totalWritten ? writeLimitAbsolute - totalWritten : 0;
        }
        totalWritten += n;
        return n;
    }
    bool connectedState = true;
    size_t writeChunk = (size_t)-1;     ///< сколько отдаём за один write()
    size_t writeLimitAbsolute = 0;      ///< 0 = без лимита всего объёма
    size_t failAfterProgress = 0;       ///< после N байт — вечный short(0)
    size_t totalWritten = 0;
};

void test_rtsp_short_write_does_not_commit_rtp_cursor() {
    RtpSendCursor cursor;
    cursor.reset(7, 1000, 123);
    PreparedRtpSend prepared = cursor.prepare(20, 48000);
    uint8_t packet[10] = {};
    TestInterleavedTransport transport;
    // Застреваем после 9 байт — writeInterleavedFull должен вернуть false.
    transport.writeChunk = 9;
    transport.failAfterProgress = 9;

    TEST_ASSERT_FALSE(writeInterleavedFull(transport, packet, sizeof(packet)));
    TEST_ASSERT_EQUAL_UINT16(7, cursor.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(1000, cursor.rtpTimestamp());
    TEST_ASSERT_EQUAL_UINT32(123, cursor.lastWriteMs());

    cursor.commit(20, prepared, 500);
    TEST_ASSERT_EQUAL_UINT16(8, cursor.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(1000, cursor.rtpTimestamp());
    TEST_ASSERT_EQUAL_UINT32(500, cursor.lastWriteMs());

    prepared = cursor.prepare(40, 48000);
    TEST_ASSERT_EQUAL_UINT32(1960, prepared.timestamp);
}

void test_rtp_cursor_ignores_opus_clock_reset() {
    RtpSendCursor cursor;
    cursor.reset(1, 0, 0);
    auto p0 = cursor.prepare(0, 48000);
    cursor.commit(0, p0, 10);
    auto p1 = cursor.prepare(20, 48000);
    cursor.commit(20, p1, 30);
    // Opus resume сбросил ms-clock в 0 — раньше давало uint underflow.
    auto p2 = cursor.prepare(0, 48000);
    TEST_ASSERT_EQUAL_UINT32(p1.timestamp + 960u, p2.timestamp);
}

class TestLifecycleSource : public AudioLifecycleSource {
public:
    explicit TestLifecycleSource(String &trace) : _trace(trace), startResult(true) {}
    void stopAudio() override { _trace += "stop;"; }
    bool startAudio() override { _trace += "start;"; return startResult; }
    bool startResult;
private:
    String &_trace;
};

class TestLifecycleConsumer : public AudioLifecycleConsumer {
public:
    TestLifecycleConsumer(String &trace, const char *name) : _trace(trace), _name(name) {}
    void pauseAudio() override { _trace += "pause"; _trace += _name; _trace += ";"; }
    void resumeAudio() override { _trace += "resume"; _trace += _name; _trace += ";"; }
private:
    String &_trace;
    const char *_name;
};

// ---------------------------------------------------------------------------
// AudioLifecycle — restart ordering, failed start
// ---------------------------------------------------------------------------

void test_audio_lifecycle_restart_orders_pause_stop_start_resume() {
    String trace;
    TestLifecycleSource source(trace);
    TestLifecycleConsumer encoder(trace, "Encoder");
    TestLifecycleConsumer local(trace, "Local");
    TestLifecycleConsumer remote(trace, "Remote");
    AudioLifecycleConsumer *consumers[] = {&encoder, &local, &remote};
    AudioLifecycleCoordinator lifecycle(source, consumers, 3);

    TEST_ASSERT_EQUAL(AudioLifecycleResult::RESTARTED, lifecycle.restart());
    TEST_ASSERT_EQUAL_STRING(
        "pauseEncoder;pauseLocal;pauseRemote;stop;start;"
        "resumeEncoder;resumeLocal;resumeRemote;",
        trace.c_str());
}

void test_audio_lifecycle_failed_start_keeps_consumers_paused() {
    String trace;
    TestLifecycleSource source(trace);
    source.startResult = false;
    TestLifecycleConsumer encoder(trace, "Encoder");
    AudioLifecycleConsumer *consumers[] = {&encoder};
    AudioLifecycleCoordinator lifecycle(source, consumers, 1);

    TEST_ASSERT_EQUAL(AudioLifecycleResult::START_FAILED, lifecycle.restart());
    TEST_ASSERT_EQUAL_STRING("pauseEncoder;stop;start;", trace.c_str());
    TEST_ASSERT_TRUE(lifecycle.isPaused());
}


// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------


void test_ntp_sync_requires_callback_not_stale_epoch() {
    NtpSyncStateMachine state(1000, 3600000, 60000);
    TEST_ASSERT_FALSE(state.isSynced(0));
    state.requestSync(100);
    TEST_ASSERT_FALSE(state.isSynced(200));
    state.onSntpSuccess(500, 1704067200000LL);
    TEST_ASSERT_TRUE(state.isSynced(600));
}

void test_wifi_recovery_uses_backoff_before_reconnect() {
    WiFiRecovery recovery(1000, 5000);
    recovery.tick(1000, true);
    recovery.tick(2000, false);
    recovery.tick(3000, false);
    TEST_ASSERT_FALSE(recovery.consumeConnectRequest());
    recovery.tick(12000, false);
    TEST_ASSERT_TRUE(recovery.consumeConnectRequest());
    TEST_ASSERT_EQUAL(WiFiRecoveryPhase::CONNECTING, recovery.phase());
}

// ---------------------------------------------------------------------------
// NetConfig — NVS schema, migration, defaults, soft apply, write errors
// ---------------------------------------------------------------------------


void test_netconfig_defaults_audio_system() {
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.0f, cfg.dspMicGain);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.hpfMode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, cfg.hpfCutoffHz);
    TEST_ASSERT_EQUAL_STRING("", cfg.timezone);
    TEST_ASSERT_FALSE_MESSAGE(cfg.scheduledReset, "scheduled reset off by default");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, cfg.calibrationOffsetDb);
    TEST_ASSERT_FALSE_MESSAGE(cfg.hmacKey.valid, "HMAC key invalid by default");
    TEST_ASSERT_EQUAL_UINT8(0, cfg.security.ledMode);
    // Range-first DSP defaults (XMOS: NS/NN 1.0 = off; AGC on for distance)
    TEST_ASSERT_TRUE_MESSAGE(cfg.dspAgcEnabled, "AGC on by default (smooth)");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, cfg.dspNsStationary);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, cfg.dspNsNonStationary);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.asroutEnabled);
    TEST_ASSERT_FALSE_MESSAGE(cfg.echoSuppressionEnabled, "echo off by default");
    TEST_ASSERT_FALSE_MESSAGE(cfg.loudspeakerPresent, "loudspeaker off by default");
    TEST_ASSERT_EQUAL_UINT8(0, cfg.attnsMode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.2f, cfg.attnsSlope);
}

void test_netconfig_schema_version_initial() {
    NetConfig::factoryReset();
    uint8_t ver = NetConfig::getStoredVersion();
    // Fresh NVS (no schema_ver key) returns 0
    TEST_ASSERT_EQUAL_UINT8(0, ver);
}

void test_netconfig_migration_v0_to_v1() {
    // Fresh NVS simulate v0 -> run migration -> should get current schema
    NetConfig::factoryReset();
    uint8_t ver = NetConfig::getStoredVersion();
    TEST_ASSERT_EQUAL_UINT8(0, ver);

    bool ok = NetConfig::runMigration();
    TEST_ASSERT_TRUE_MESSAGE(ok, "migration v0->v4 must succeed");

    ver = NetConfig::getStoredVersion();
    TEST_ASSERT_EQUAL_UINT8(NVS_SCHEMA_VERSION, ver);
}

void test_netconfig_migration_v1_noop() {
    // If already at current schema, migration should be no-op
    NetConfig::factoryReset();
    NetConfig::runMigration();
    uint8_t ver = NetConfig::getStoredVersion();
    TEST_ASSERT_EQUAL_UINT8(NVS_SCHEMA_VERSION, ver);

    bool ok = NetConfig::runMigration();
    TEST_ASSERT_TRUE_MESSAGE(ok, "migration at current schema must be no-op");
    ver = NetConfig::getStoredVersion();
    TEST_ASSERT_EQUAL_UINT8(NVS_SCHEMA_VERSION, ver);
}

void test_netconfig_migration_v1_defaults() {
    // After migration v0->v1, load should produce valid defaults
    NetConfig::factoryReset();
    NetConfig::runMigration();

    NetConfigData cfg;
    NetConfig::load(cfg);
    // Cloud must be opt-in false (fresh NVS)
    TEST_ASSERT_FALSE_MESSAGE(cfg.mqttEnabled, "migrated: mqtt opt-in");
    // Audio defaults
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.0f, cfg.dspMicGain);
    TEST_ASSERT_FALSE_MESSAGE(cfg.hpfEnabled, "migrated: hpf disabled");
    TEST_ASSERT_EQUAL_UINT8(0, cfg.hpfMode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, cfg.hpfCutoffHz);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------


void test_netconfig_load_save_roundtrip() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::load(cfg);

    // Modify some fields
    cfg.mqttEnabled = true;
    cfg.mqttPort = 1883;
    cfg.dspMicGain = 3.5f;
    cfg.hpfMode = 1;  // on70
    NetConfig::syncHpfFields(cfg);
    cfg.scheduledReset = true;
    strncpy(cfg.timezone, "Europe/Moscow", sizeof(cfg.timezone));
    cfg.calibrationOffsetDb = -3.0f;
    cfg.security.ledMode = LED_MODE_OFF;

    bool ok = NetConfig::save(cfg);
    TEST_ASSERT_TRUE_MESSAGE(ok, "save must succeed");

    // Reload
    NetConfigData loaded;
    NetConfig::load(loaded);

    TEST_ASSERT_TRUE_MESSAGE(loaded.mqttEnabled, "roundtrip mqtt");
    TEST_ASSERT_EQUAL_UINT16(1883, loaded.mqttPort);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.5f, loaded.dspMicGain);
    TEST_ASSERT_TRUE_MESSAGE(loaded.hpfEnabled, "roundtrip hpf on");
    TEST_ASSERT_EQUAL_UINT8(1, loaded.hpfMode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, loaded.hpfCutoffHz);
    TEST_ASSERT_TRUE_MESSAGE(loaded.scheduledReset, "roundtrip sched reset");
    TEST_ASSERT_EQUAL_STRING("Europe/Moscow", loaded.timezone);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -3.0f, loaded.calibrationOffsetDb);
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_OFF, loaded.security.ledMode);
}

void test_netconfig_validation_bounds() {
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);
    const uint16_t mqttPortBefore = cfg.mqttPort;

    // fromJson should reject out-of-bounds values
    StaticJsonDocument<512> doc;
    doc["dsp_mic_gain"] = 5000.0f;       // out of bounds
    doc["hpf_mode"] = 9;                 // out of bounds
    doc["hpf_cutoff_hz"] = 9000.0f;   // ignored (derived from mode)
    doc["calibration_offset_db"] = 50.0f; // out of bounds (±40)
    doc["detection_threshold_ratio"] = 2.0f; // out of bounds
    doc["detection_threshold_rms"] = -1.0f;  // out of bounds
    doc["mqtt_port"] = 70000;          // wrap risk
    doc["grpc_port"] = -1;

    // These should keep defaults
    NetConfig::fromJson(doc.as<JsonVariantConst>(), cfg);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.0f, cfg.dspMicGain);       // unchanged
    TEST_ASSERT_EQUAL_UINT8(0, cfg.hpfMode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, cfg.hpfCutoffHz);   // unchanged
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, cfg.calibrationOffsetDb); // unchanged
    TEST_ASSERT_EQUAL_UINT16(mqttPortBefore, cfg.mqttPort);
}

// ---------------------------------------------------------------------------
// CommandAuth — signed payload, expiry, replay
// ---------------------------------------------------------------------------

void test_command_auth_payload_legacy_and_nonce() {
    char buf[256];
    StaticJsonDocument<64> val;
    val.set(42);

    size_t n = CommandAuth::buildSignedPayload(buf, sizeof(buf), "mute", 1000,
                                               "", val.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("mute|1000|42", buf);

    n = CommandAuth::buildSignedPayload(buf, sizeof(buf), "mute", 1000,
                                        "abc", val.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("mute|1000|abc|42", buf);

    TEST_ASSERT_TRUE(CommandAuth::constTimeHexEq("aa", "aa", 2));
    TEST_ASSERT_FALSE(CommandAuth::constTimeHexEq("aa", "ab", 2));
}

void test_netconfig_soft_apply_no_reboot() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);

    // Audio-only change: no reboot needed
    cfg.dspMicGain = 2.0f;
    SoftApplyResult res = NetConfig::softApply(cfg, false, false, false, true, false, false);
    TEST_ASSERT_TRUE_MESSAGE(res.success, "soft apply must succeed");
    TEST_ASSERT_FALSE_MESSAGE(res.rebootRecommended, "audio change no reboot");
    TEST_ASSERT_NULL_MESSAGE(res.errorMsg, "no error");
}

void test_netconfig_soft_apply_reboot_required() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);

    // Network change: reboot needed
    cfg.mqttEnabled = true;
    cfg.mqttHost[0] = '1';
    SoftApplyResult res = NetConfig::softApply(cfg, true, false, false, false, false, false);
    TEST_ASSERT_TRUE_MESSAGE(res.success, "soft apply must succeed");
    TEST_ASSERT_TRUE_MESSAGE(res.rebootRecommended, "mqtt change needs reboot");
}

void test_netconfig_soft_apply_rtsp_reboot() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);

    cfg.rtspRemoteEnabled = true;
    cfg.rtspRemotePort = 8555;
    SoftApplyResult res = NetConfig::softApply(cfg, false, true, false, false, false, false);
    TEST_ASSERT_TRUE_MESSAGE(res.success, "soft apply must succeed");
    TEST_ASSERT_TRUE_MESSAGE(res.rebootRecommended, "rtsp change needs reboot");
}

void test_netconfig_soft_apply_ntp_no_reboot() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);

    strncpy(cfg.ntpHost, "time.google.com", sizeof(cfg.ntpHost));
    SoftApplyResult res = NetConfig::softApply(cfg, false, false, true, false, false, false);
    TEST_ASSERT_TRUE_MESSAGE(res.success, "soft apply must succeed");
    TEST_ASSERT_TRUE_MESSAGE(res.rebootRecommended, "ntp change needs reboot");
}

void test_netconfig_write_error_simulation() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);

    // Enable write failure simulation
    g_prefs_write_fail = true;

    // Save should fail
    bool ok = NetConfig::save(cfg);
    TEST_ASSERT_FALSE_MESSAGE(ok, "save must fail with write error");

    // Soft apply should also fail
    cfg.dspMicGain = 2.5f;
    SoftApplyResult res = NetConfig::softApply(cfg, false, false, false, true, false, false);
    TEST_ASSERT_FALSE_MESSAGE(res.success, "soft apply must fail on write error");
    TEST_ASSERT_NOT_NULL_MESSAGE(res.errorMsg, "error message must be set");

    // Reset flag
    g_prefs_write_fail = false;
}

void test_netconfig_factory_reset() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::load(cfg);

    // After factory reset, all should be opt-in defaults
    TEST_ASSERT_FALSE_MESSAGE(cfg.mqttEnabled, "factory reset: mqtt off");
    TEST_ASSERT_FALSE_MESSAGE(cfg.rtspRemoteEnabled, "factory reset: rtsp off");
    TEST_ASSERT_FALSE_MESSAGE(cfg.mqttHaDiscovery, "factory reset: ha off");
    TEST_ASSERT_FALSE_MESSAGE(cfg.scheduledReset, "factory reset: sched reset off");
}

void test_netconfig_network_reset_preserves_audio() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::load(cfg);

    // Set audio and network settings
    cfg.dspMicGain = 4.0f;
    cfg.hpfEnabled = false;
    cfg.mqttEnabled = true;
    cfg.rtspRemoteEnabled = true;
    NetConfig::save(cfg);

    // Network reset
    bool ok = NetConfig::networkReset();
    TEST_ASSERT_TRUE_MESSAGE(ok, "network reset must succeed");

    // Reload
    NetConfigData loaded;
    NetConfig::load(loaded);

    // Audio settings preserved
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, loaded.dspMicGain);
    TEST_ASSERT_FALSE_MESSAGE(loaded.hpfEnabled, "hpf preserved");

    // Network settings cleared to defaults (opt-in)
    TEST_ASSERT_FALSE_MESSAGE(loaded.mqttEnabled, "network reset: mqtt off");
    TEST_ASSERT_FALSE_MESSAGE(loaded.rtspRemoteEnabled, "network reset: rtsp off");
}

void test_netconfig_to_json_roundtrip() {
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);
    cfg.dspMicGain = 2.5f;
    cfg.hpfMode = 4;  // on180
    NetConfig::syncHpfFields(cfg);
    cfg.mqttEnabled = true;
    strncpy(cfg.mqttHost, "mqtt.example.com", sizeof(cfg.mqttHost));
    cfg.mqttPort = 8883;
    strncpy(cfg.mqttPass, "secret123", sizeof(cfg.mqttPass));
    cfg.scheduledReset = true;
    cfg.calibrationOffsetDb = -2.5f;

    // Serialize to JSON
    StaticJsonDocument<2048> doc;
    JsonObject obj = doc.to<JsonObject>();
    NetConfig::toJson(obj, cfg, true);

    // Verify fields
    TEST_ASSERT_TRUE(obj["mqtt_enabled"]);
    TEST_ASSERT_EQUAL_STRING("mqtt.example.com", obj["mqtt_host"]);
    TEST_ASSERT_EQUAL_INT(8883, obj["mqtt_port"]);
    TEST_ASSERT_EQUAL_STRING("********", obj["mqtt_pass"]); // masked
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.5f, obj["dsp_mic_gain"].as<float>());
    TEST_ASSERT_TRUE(obj["hpf_enabled"]);
    TEST_ASSERT_EQUAL_INT(4, obj["hpf_mode"].as<int>());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, obj["hpf_cutoff_hz"].as<float>());
    TEST_ASSERT_TRUE(obj["scheduled_reset"]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -2.5f, obj["calibration_offset_db"].as<float>());

    // Deserialize from JSON into cfg2 (which already has correct password)
    NetConfigData cfg2;
    NetConfig::applyDefaults(cfg2);
    // Preserve secret from original cfg before fromJson parses masked values
    strncpy(cfg2.mqttPass, cfg.mqttPass, sizeof(cfg2.mqttPass));
    NetConfig::fromJson(obj, cfg2);

    TEST_ASSERT_TRUE_MESSAGE(cfg2.mqttEnabled, "json roundtrip mqtt");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.5f, cfg2.dspMicGain);
    TEST_ASSERT_TRUE_MESSAGE(cfg2.hpfEnabled, "json roundtrip hpf");
    TEST_ASSERT_EQUAL_UINT8(4, cfg2.hpfMode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, cfg2.hpfCutoffHz);
    TEST_ASSERT_TRUE_MESSAGE(cfg2.scheduledReset, "json roundtrip sched");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -2.5f, cfg2.calibrationOffsetDb);
    // Password masked in JSON -> should keep existing
    TEST_ASSERT_EQUAL_STRING("secret123", cfg2.mqttPass);
}

void test_netconfig_hmac_key_roundtrip() {
    NetConfig::factoryReset();
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);

    // Set a 32-byte HMAC key
    uint8_t keyBytes[32];
    for (int i = 0; i < 32; i++) keyBytes[i] = (uint8_t)i;
    memcpy(cfg.hmacKey.bytes, keyBytes, 32);
    cfg.hmacKey.valid = true;

    bool ok = NetConfig::save(cfg);
    TEST_ASSERT_TRUE_MESSAGE(ok, "hmac key save");

    NetConfigData loaded;
    NetConfig::load(loaded);
    TEST_ASSERT_TRUE_MESSAGE(loaded.hmacKey.valid, "hmac key valid after load");
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_UINT8(keyBytes[i], loaded.hmacKey.bytes[i]);
    }
}

void test_netconfig_led_mode_default() {
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);
    TEST_ASSERT_EQUAL_UINT8(LED_MODE_STATUS, cfg.security.ledMode);
}

void setUp() {
    test_millis_set(0);
    RtspMicTest::g_vadActive     = false;
    RtspMicTest::g_doaAzimuth    = 0;
    RtspMicTest::g_speechDetected = 0.0f;
    RtspMicTest::g_vadResult     = XVF3800_Result::OK;
    RtspMicTest::g_doaResult     = XVF3800_Result::OK;
    RtspMicTest::g_ntpSynced     = true;
    RtspMicTest::g_epochMillis   = 1704067200000LL;
    RtspMicTest::g_rtspStreaming = false;
}

void tearDown() {}

void test_webcred_validate_user_pass() {
    TEST_ASSERT_TRUE(WebCredentials::validateUser("admin"));
    TEST_ASSERT_TRUE(WebCredentials::validateUser("a_b.9-x"));
    TEST_ASSERT_FALSE(WebCredentials::validateUser(""));
    TEST_ASSERT_FALSE(WebCredentials::validateUser("bad user"));
    TEST_ASSERT_FALSE(WebCredentials::validateUser("x@y"));
    TEST_ASSERT_TRUE(WebCredentials::validatePass("12345678"));
    TEST_ASSERT_FALSE(WebCredentials::validatePass("short"));
    TEST_ASSERT_FALSE(WebCredentials::validatePass("bad:pass1"));
}

void test_webcred_parse_basic_auth() {
    char user[33];
    char pass[64];
    TEST_ASSERT_TRUE(WebCredentials::parseBasicAuthHeader(
        "Authorization: Basic YWRtaW46cnRzcG1pYzEyIQ==", user, sizeof(user), pass, sizeof(pass)));
    TEST_ASSERT_EQUAL_STRING("admin", user);
    TEST_ASSERT_EQUAL_STRING("rtspmic12!", pass);
    TEST_ASSERT_TRUE(WebCredentials::parseBasicAuthHeader(
        "Basic YWRtaW46cnRzcG1pYzEyIQ==", user, sizeof(user), pass, sizeof(pass)));
    TEST_ASSERT_EQUAL_STRING("admin", user);
    TEST_ASSERT_FALSE(WebCredentials::parseBasicAuthHeader(nullptr, user, sizeof(user), pass, sizeof(pass)));
    TEST_ASSERT_FALSE(WebCredentials::parseBasicAuthHeader("Bearer x", user, sizeof(user), pass, sizeof(pass)));
}

void test_webcred_verify_basic_auth() {
    TEST_ASSERT_TRUE(WebCredentials::verifyBasicAuth(
        "Authorization: Basic YWRtaW46cnRzcG1pYzEyIQ==", "admin", "rtspmic12!"));
    TEST_ASSERT_FALSE(WebCredentials::verifyBasicAuth(
        "Authorization: Basic YWRtaW46cnRzcG1pYzEyIQ==", "admin", "wrongpass"));
    TEST_ASSERT_FALSE(WebCredentials::verifyBasicAuth("", "admin", "rtspmic12!"));
}

void test_webcred_ct_eq_and_default_pw() {
    TEST_ASSERT_TRUE(WebCredentials::ctEq("abc", "abc"));
    TEST_ASSERT_FALSE(WebCredentials::ctEq("abc", "abd"));
    TEST_ASSERT_FALSE(WebCredentials::ctEq("abc", "ab"));
    // Fresh NVS mock → defaults → default password flag set.
    TEST_ASSERT_TRUE(WebCredentials::isDefaultPassword());
}

void test_telemetry_public_v1_no_sensitive() {
    RtspMicTest::g_ntpSynced   = true;
    RtspMicTest::g_epochMillis = 1704067200000LL;

    XVF3800_I2C xvf;
    xvf.begin();
    XVF3800_Cache cache(&xvf);
    NTPClient ntp;
    RTSPClient rtsp;

    TelemetryBuilder::init(nullptr, &cache, &ntp, &rtsp, "LOC001");
    String json = TelemetryBuilder::buildLocator();

    DynamicJsonDocument doc(1536);
    TEST_ASSERT_TRUE(deserializeJson(doc, json.c_str()) == DeserializationError::Ok);
    TEST_ASSERT_EQUAL_STRING("rtsp-mic.public.v1", doc["schema"]);
    TEST_ASSERT_EQUAL_STRING("LOC001", doc["node_id"]);
    TEST_ASSERT_TRUE(doc.containsKey("audio"));
    TEST_ASSERT_TRUE(doc.containsKey("system"));
    TEST_ASSERT_TRUE(doc["system"].containsKey("wifi_rssi"));
    // Public status must not leak MAC, SSID, or full telemetry.
    TEST_ASSERT_FALSE(doc.containsKey("mac"));
    TEST_ASSERT_FALSE(doc.containsKey("ssid"));
    TEST_ASSERT_FALSE(doc.containsKey("wifi_ssid"));
    TEST_ASSERT_FALSE(doc["system"].containsKey("mac"));
    TEST_ASSERT_FALSE(doc["system"].containsKey("ssid"));
    TEST_ASSERT_FALSE(doc["audio"].containsKey("doa_deg"));
}







void test_netconfig_sanitize_loudspeaker_off() {
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);
    cfg.loudspeakerPresent = false;
    cfg.echoSuppressionEnabled = true;
    cfg.asroutEnabled = 0;
    NetConfig::sanitizeLoudspeakerOff(cfg);
    TEST_ASSERT_FALSE(cfg.echoSuppressionEnabled);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.asroutEnabled);
}

void test_netconfig_sys_delay_allows_negative() {
    NetConfigData cfg;
    NetConfig::applyDefaults(cfg);
    DynamicJsonDocument doc(128);
    doc["sys_delay"] = -40;
    TEST_ASSERT_TRUE(NetConfig::fromJson(doc.as<JsonVariantConst>(), cfg));
    TEST_ASSERT_EQUAL_INT32(-40, cfg.sysDelaySamples);
}

void test_dsp_level_compensate_includes_all_path_gains() {
    DspLevelCompParams p{};
    p.agcGainDb = 20.0f;          // ×10
    p.agcEnabled = true;
    p.micGainLin = 10.0f;         // +20 dB
    p.asroutGainLin = 2.0f;       // +6.02 dB
    p.asroutEnabled = true;
    p.attnsMode = 0;
    p.attnsNominal = 1.0f;
    p.attnsSlope = 0.2f;
    p.speechActive = false;
    p.agcGainInitLin = 32.0f;
    p.softwareGainLin = 2.0f;     // +6.02 dB digital

    // AGC 20 + mic ~20 + asrout ~6 + software ~6 ≈ 52 dB
    float g = dspEffectiveGainDb(p);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 52.04f, g);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -56.02f, dspCompensateLevelDb(-3.98f, g));
}

void test_dsp_level_compensate_attns_in_nonspeech() {
    DspLevelCompParams p{};
    p.agcGainDb = 46.0f;  // high AGC (~×200) — дальность / тишина
    p.agcEnabled = true;
    p.micGainLin = 1.0f;
    p.asroutGainLin = 1.0f;
    p.asroutEnabled = true;
    p.attnsMode = 1;
    p.attnsNominal = 1.0f;
    p.attnsSlope = 0.2f;
    p.speechActive = false;
    p.agcGainInitLin = 32.0f;

    float gOn = dspEffectiveGainDb(p);
    p.speechActive = true;
    float gSpeech = dspEffectiveGainDb(p);
    // High AGC: ATTNS factor < 1 → effective gain ниже, чем при speech
    TEST_ASSERT_TRUE(gOn < gSpeech);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 46.0f, gSpeech);  // только AGC
}

// ---------------------------------------------------------------------------
// WsTicketAuth — HMAC WS tickets + NonceStore
// ---------------------------------------------------------------------------

static const uint8_t kWsTicketTestKey[32] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

void test_ws_ticket_valid_roundtrip() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0xC0A8010A;  // 192.168.1.10
    WsTicketAuth::NonceStore used;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 1000, 10000, 0xA5A5A5A5,
                                         ticket, sizeof(ticket)));
    TEST_ASSERT_EQUAL(WsTicketAuth::kTicketLen - 1, strlen(ticket));
    TEST_ASSERT_EQUAL_CHAR('2', ticket[0]);
    TEST_ASSERT_TRUE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                          ip, 1000, ticket, &used));
    // Single-use: replay rejected
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 1000, ticket, &used));
}

void test_ws_ticket_rejects_wrong_ip() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0x0A000001;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 0, 10000, 1, ticket, sizeof(ticket)));
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip + 1, 0, ticket));
}

void test_ws_ticket_rejects_tampered_payload() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0x0A000002;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 0, 10000, 2, ticket, sizeof(ticket)));
    ticket[5] ^= 0x1;
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 0, ticket));
}

void test_ws_ticket_rejects_tampered_mac() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0x0A000003;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 0, 10000, 3, ticket, sizeof(ticket)));
    ticket[WsTicketAuth::kPayloadLen] ^= 0x1;
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 0, ticket));
}

void test_ws_ticket_rejects_expired() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0x0A000004;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 1000, 10000, 4, ticket, sizeof(ticket)));
    TEST_ASSERT_TRUE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                          ip, 10999, ticket));
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 11000, ticket));
}

void test_ws_ticket_rejects_wrong_key() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0x0A000005;
    uint8_t otherKey[32];
    memcpy(otherKey, kWsTicketTestKey, sizeof(otherKey));
    otherKey[0] ^= 0xff;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 0, 10000, 5, ticket, sizeof(ticket)));
    TEST_ASSERT_FALSE(WsTicketAuth::verify(otherKey, sizeof(otherKey), ip, 0, ticket));
}

void test_ws_ticket_rejects_bad_length_and_version() {
    char ticket[WsTicketAuth::kTicketLen];
    const uint32_t ip = 0x0A000006;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         ip, 0, 10000, 6, ticket, sizeof(ticket)));
    ticket[0] = '1';  // old version rejected
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 0, ticket));
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 0, "short"));
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           ip, 0, nullptr));
}

void test_ws_ticket_issues_beyond_legacy_slot_cap() {
    char tickets[12][WsTicketAuth::kTicketLen];
    WsTicketAuth::NonceStore used;
    for (uint32_t i = 0; i < 12; ++i) {
        TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                             0x0A000010 + i, 0, 10000, 100 + i,
                                             tickets[i], sizeof(tickets[i])));
        TEST_ASSERT_TRUE(tickets[i][0] != '\0');
        TEST_ASSERT_TRUE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                              0x0A000010 + i, 0, tickets[i], &used));
    }
}

void test_ws_ticket_nonce_reuse_rejected() {
    char ticket[WsTicketAuth::kTicketLen];
    WsTicketAuth::NonceStore used;
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         0x0A000020, 0, 10000, 42, ticket, sizeof(ticket)));
    TEST_ASSERT_TRUE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                          0x0A000020, 0, ticket, &used));
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           0x0A000020, 0, ticket, &used));
}

void test_ws_ticket_nonce_store_full_rejects() {
    WsTicketAuth::NonceStore used;
    char ticket[WsTicketAuth::kTicketLen];
    for (uint32_t i = 0; i < WsTicketAuth::kNonceSlots; ++i) {
        TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                             0x0A001000 + i, 0, 10000, 1000 + i,
                                             ticket, sizeof(ticket)));
        TEST_ASSERT_TRUE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                              0x0A001000 + i, 0, ticket, &used));
    }
    TEST_ASSERT_EQUAL_UINT8(WsTicketAuth::kNonceSlots, used.count);
    TEST_ASSERT_TRUE(WsTicketAuth::issue(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                         0x0A0010FF, 0, 10000, 0xDEAD,
                                         ticket, sizeof(ticket)));
    // Fail-closed: full store must not evict a live nonce to accept a new one.
    TEST_ASSERT_FALSE(WsTicketAuth::verify(kWsTicketTestKey, sizeof(kWsTicketTestKey),
                                           0x0A0010FF, 0, ticket, &used));
    TEST_ASSERT_EQUAL_UINT8(WsTicketAuth::kNonceSlots, used.count);
}

// ── NonceStore: purge boundary (expiry == nowMs) ──
void test_nonce_store_purge_expired_exact_boundary() {
    WsTicketAuth::NonceStore ns;
    TEST_ASSERT_TRUE(ns.consume(1, 1000, 0));
    TEST_ASSERT_TRUE(ns.consume(2, 2000, 0));
    TEST_ASSERT_TRUE(ns.consume(3, 3000, 0));
    // At nowMs=999 the nonce with expiry=1000 is NOT expired (999 < 1000).
    ns.purgeExpired(999);
    TEST_ASSERT_EQUAL_UINT8(3, ns.count);
    // At nowMs=1000 the first nonce (expiry=1000) IS expired (1000 >= 1000).
    ns.purgeExpired(1000);
    TEST_ASSERT_EQUAL_UINT8(2, ns.count);
    TEST_ASSERT_FALSE(ns.seen(1));   // nonce #1 purged
    TEST_ASSERT_TRUE(ns.seen(2));    // nonce #2 still valid
    // Fill to full (30 more, total 32) and confirm consume fails.
    for (uint32_t i = 0; i < 30; ++i) {
        TEST_ASSERT_TRUE(ns.consume(100 + i, 5000, 1001));
    }
    TEST_ASSERT_EQUAL_UINT8(32, ns.count);
    TEST_ASSERT_FALSE(ns.consume(99, 10000, 1001));  // fail-closed
}

// ── ReconnectBackoff: ceiling at max, no overflow ──
void test_reconnect_backoff_ceiling() {
    ReconnectBackoff backoff(1000, 30000);
    for (int i = 0; i < 10; ++i) {
        backoff.recordFailure(0);
        TEST_ASSERT_TRUE(backoff.delayMs() <= 30000);
    }
    TEST_ASSERT_EQUAL_UINT32(30000, backoff.delayMs());
    // Reset after success
    backoff.recordSuccess();
    TEST_ASSERT_EQUAL_UINT32(1000, backoff.delayMs());
}

// ── CommandAuth: expired nonce, legacy, replay window ──
void test_command_auth_expired_nonce() {
    StaticJsonDocument<64> val;
    val.set(42);
    char buf[256];
    // Nonce "abc" with ts=1000000 (epoch past)
    size_t n = CommandAuth::buildSignedPayload(buf, sizeof(buf), "mute", 1000000,
                                               "abc", val.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("mute|1000000|abc|42", buf);
    // Legacy (empty nonce) still works
    n = CommandAuth::buildSignedPayload(buf, sizeof(buf), "mute", 1000000,
                                        "", val.as<JsonVariantConst>());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("mute|1000000|42", buf);
    // Null payload still builds (no JSON value)
    n = CommandAuth::buildSignedPayload(buf, sizeof(buf), "mute", 1000000,
                                        "abc", JsonVariantConst());
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("mute|1000000|abc|", buf);
}


// ── ValidateUtil: URL and hostname ──
void test_validate_url_https_ok() {
    TEST_ASSERT_TRUE(ValidateUtil::validateUrl("https://api.example.com/api", 64));
    TEST_ASSERT_TRUE(ValidateUtil::validateUrl("https://192.168.1.10:8443/path", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl(nullptr, 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("http://evil.com/health", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("ftp://bad", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("https://ok\"quote", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("https://ok'single", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("https://ok`backtick", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("https://ok\\backslash", 64));
    // Exact fit: 11 chars + NUL OK at maxLen=12
    TEST_ASSERT_TRUE(ValidateUtil::validateUrl("https://x.y", 12));
    TEST_ASSERT_FALSE(ValidateUtil::validateUrl("https://x.yz", 12));  // too long
}

void test_validate_hostname_ok() {
    TEST_ASSERT_TRUE(ValidateUtil::validateHostname("api.example.com", 64));
    TEST_ASSERT_TRUE(ValidateUtil::validateHostname("192.168.1.10", 64));
    TEST_ASSERT_TRUE(ValidateUtil::validateHostname("localhost", 64));
    TEST_ASSERT_TRUE(ValidateUtil::validateHostname("my-host:8883", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateHostname(nullptr, 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateHostname("", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateHostname("bad host", 64));   // space
    TEST_ASSERT_FALSE(ValidateUtil::validateHostname("host>bad", 64));
    TEST_ASSERT_FALSE(ValidateUtil::validateHostname("host`bad", 64));
}

// ── CommandAuth: HMAC sign + verify ──
static const uint8_t kCmdTestKey[32] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
};

void test_command_auth_hmac_roundtrip() {
    char hex1[65], hex2[65];
    const char *payload = "mute|1700000000|abcdef12|42";
    // Same payload → same hex (deterministic)
    TEST_ASSERT_TRUE(CommandAuth::hmacHex(kCmdTestKey, 32, payload, hex1, sizeof(hex1)));
    TEST_ASSERT_TRUE(CommandAuth::hmacHex(kCmdTestKey, 32, payload, hex2, sizeof(hex2)));
    TEST_ASSERT_EQUAL_STRING(hex1, hex2);
    TEST_ASSERT_EQUAL_UINT8(64, strlen(hex1));
    // Wrong key gives different hex
    uint8_t wrong[32];
    memcpy(wrong, kCmdTestKey, 32);
    wrong[0] ^= 0xff;
    TEST_ASSERT_TRUE(CommandAuth::hmacHex(wrong, 32, payload, hex2, sizeof(hex2)));
    TEST_ASSERT_FALSE(CommandAuth::constTimeHexEq(hex1, hex2, 64));
}

void test_command_auth_verify_hmac() {
    char hex[65];
    const char *payload = "mute|1700000000|abcdef12|42";
    TEST_ASSERT_TRUE(CommandAuth::hmacHex(kCmdTestKey, 32, payload, hex, sizeof(hex)));
    TEST_ASSERT_TRUE(CommandAuth::verifyHmac(kCmdTestKey, 32, payload, hex));
    // Tampered sig rejected
    hex[0] ^= 0x1;
    TEST_ASSERT_FALSE(CommandAuth::verifyHmac(kCmdTestKey, 32, payload, hex));
    hex[0] ^= 0x1;
    TEST_ASSERT_TRUE(CommandAuth::verifyHmac(kCmdTestKey, 32, payload, hex));
    // Wrong key rejected
    uint8_t wrong[32];
    memcpy(wrong, kCmdTestKey, 32);
    wrong[0] ^= 0xff;
    TEST_ASSERT_FALSE(CommandAuth::verifyHmac(wrong, 32, payload, hex));
    // Null/empty rejection
    TEST_ASSERT_FALSE(CommandAuth::verifyHmac(nullptr, 32, payload, hex));
    TEST_ASSERT_FALSE(CommandAuth::verifyHmac(kCmdTestKey, 32, nullptr, hex));
    TEST_ASSERT_FALSE(CommandAuth::verifyHmac(kCmdTestKey, 0, payload, hex));
}

// ── MQTTManager nonce cache (host-testable via testOnly) ──
void test_mqtt_nonce_cache_rejects_duplicate() {
    MQTTManager mqtt;
    const char *nonce = "abcdef12345678";
    test_millis_set(1000);
    TEST_ASSERT_TRUE(mqtt.testOnly_rememberNonce(nonce, 1000));
    TEST_ASSERT_TRUE(mqtt.testOnly_wasNonceSeen(nonce));
    TEST_ASSERT_FALSE(mqtt.testOnly_wasNonceSeen("zzzzzzzzzzzzzzzz"));
}

void test_mqtt_nonce_expires_after_ttl() {
    MQTTManager mqtt;
    const char *nonce = "abcdef12345678";
    test_millis_set(0);
    TEST_ASSERT_TRUE(mqtt.testOnly_rememberNonce(nonce, 0));
    test_millis_set(30000);
    TEST_ASSERT_TRUE(mqtt.testOnly_wasNonceSeen(nonce));
    test_millis_set(100000);
    TEST_ASSERT_FALSE(mqtt.testOnly_wasNonceSeen(nonce));
}

// ── MQTTManager nonce: fail-closed when 64 live nonces ──
void test_mqtt_nonce_cache_overflow_is_fail_closed() {
    MQTTManager mqtt;
    char nonce[20];
    test_millis_set(0);
    for (size_t i = 0; i < 64; ++i) {
        snprintf(nonce, sizeof(nonce), "abcdef%08zu", i);
        TEST_ASSERT_TRUE(mqtt.testOnly_rememberNonce(nonce, 0));
    }
    // 65th nonce — fail-closed (all slots live)
    snprintf(nonce, sizeof(nonce), "abcdef%08zu", (size_t)64);
    TEST_ASSERT_FALSE(mqtt.testOnly_rememberNonce(nonce, 0));
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_webcred_validate_user_pass);
    RUN_TEST(test_webcred_parse_basic_auth);
    RUN_TEST(test_webcred_verify_basic_auth);
    RUN_TEST(test_webcred_ct_eq_and_default_pw);
    RUN_TEST(test_ws_ticket_valid_roundtrip);
    RUN_TEST(test_ws_ticket_rejects_wrong_ip);
    RUN_TEST(test_ws_ticket_rejects_tampered_payload);
    RUN_TEST(test_ws_ticket_rejects_tampered_mac);
    RUN_TEST(test_ws_ticket_rejects_expired);
    RUN_TEST(test_ws_ticket_rejects_wrong_key);
    RUN_TEST(test_ws_ticket_rejects_bad_length_and_version);
    RUN_TEST(test_ws_ticket_issues_beyond_legacy_slot_cap);
    RUN_TEST(test_ws_ticket_nonce_reuse_rejected);
    RUN_TEST(test_ws_ticket_nonce_store_full_rejects);
    RUN_TEST(test_nonce_store_purge_expired_exact_boundary);
    RUN_TEST(test_telemetry_public_v1_no_sensitive);
    RUN_TEST(test_netconfig_sanitize_loudspeaker_off);
    RUN_TEST(test_netconfig_sys_delay_allows_negative);
    RUN_TEST(test_dsp_level_compensate_includes_all_path_gains);
    RUN_TEST(test_dsp_level_compensate_attns_in_nonspeech);
    RUN_TEST(test_dsp_level_compensate_agc_off_ignores_chip_gain);

    // ---------------------------------------------------------------------------
    // HighPassFilter
    // ---------------------------------------------------------------------------
    RUN_TEST(test_hpf_disabled_passthrough);
    RUN_TEST(test_hpf_attenuates_dc);
    RUN_TEST(test_hpf_process_block);
    RUN_TEST(test_hpf_set_cutoff);

    RUN_TEST(test_audio_convert_stereo_to_mono);
    RUN_TEST(test_audio_convert_gain_clipping);

    RUN_TEST(test_xvf_cache_reads_vad_doa);
    RUN_TEST(test_xvf_cache_skips_failed_i2c);

    RUN_TEST(test_telemetry_v2_standard_fields);
    RUN_TEST(test_telemetry_v2_doa_uses_chip_azimuth);
    RUN_TEST(test_telemetry_v2_extended_webui_fields);
    RUN_TEST(test_telemetry_no_silent_truncation);
    RUN_TEST(test_mel_copy_recent_frames_chrono);

    RUN_TEST(test_mel_begin_end);
    RUN_TEST(test_mel_compute_frame_unity_sine);
    RUN_TEST(test_mel_push_get_frame_count);
    RUN_TEST(test_mel_push_multiple_frames);
    RUN_TEST(test_mel_get_spectrogram_buffer);
    RUN_TEST(test_mel_shutdown_rejects_new_writer_work);
    RUN_TEST(test_mel_reset_for_restart_reenables_writers);

    RUN_TEST(test_thermal_initial_state);
    RUN_TEST(test_thermal_throttle_at_85c);
    RUN_TEST(test_thermal_no_throttle_at_75c);
    RUN_TEST(test_thermal_shutdown_at_95c);
    RUN_TEST(test_thermal_cooldown_recovery);
    RUN_TEST(test_thermal_no_shutdown_wait_still_hot);
    RUN_TEST(test_thermal_persistent_latch);
    RUN_TEST(test_ringbuf_health_no_overflow);
    RUN_TEST(test_ringbuf_health_overflow_triggers_recovery);

    RUN_TEST(test_ws_telemetry_gate_backpressure);


    RUN_TEST(test_slm_initial_state);
    RUN_TEST(test_slm_silence_stays_low);
    RUN_TEST(test_slm_loud_signal);
    RUN_TEST(test_slm_a_weighting_reduces_low_freq);
    RUN_TEST(test_slm_fast_vs_slow);
    RUN_TEST(test_slm_reset_la_eq);
    RUN_TEST(test_slm_disabled_skips);

    RUN_TEST(test_led_indicator_initial_state);
    RUN_TEST(test_led_indicator_static_on);
    RUN_TEST(test_led_indicator_static_off);
    RUN_TEST(test_led_indicator_blink_startup);
    RUN_TEST(test_led_indicator_blink_error);
    RUN_TEST(test_led_indicator_blink_net_fail);
    RUN_TEST(test_led_indicator_blink_warning);
    RUN_TEST(test_led_indicator_level);

    RUN_TEST(test_watchdog_initial_state);
    RUN_TEST(test_watchdog_kick_keeps_alive);
    RUN_TEST(test_watchdog_timeout_stalls);
    RUN_TEST(test_watchdog_kick_resets_stall);
    RUN_TEST(test_watchdog_esp_restart_after_max_stall);
    RUN_TEST(test_watchdog_disabled_no_action);
    RUN_TEST(test_watchdog_no_kick_never_stalled);

    RUN_TEST(test_audio_fanout_consumers_are_independent);
    RUN_TEST(test_audio_fanout_overflow_drops_oldest_per_consumer);
    RUN_TEST(test_pcm_accumulator_preserves_partial_chunks);
    RUN_TEST(test_pcm_frame_clock_tracks_dropped_encoded_frame_time);
    RUN_TEST(test_task_lifecycle_exit_owner_clears_handle);
    RUN_TEST(test_task_lifecycle_start_failure_is_reported);
    RUN_TEST(test_freertos_task_handshake_blocks_until_real_ack);
    RUN_TEST(test_rtp_stream_guard_disconnects_before_consuming);
    RUN_TEST(test_rtp_stream_guard_rejects_partial_write);
    RUN_TEST(test_reconnect_backoff_grows_and_resets);
    RUN_TEST(test_reconnect_backoff_ceiling);
    RUN_TEST(test_rtsp_intermediate_timeout_requires_reconnect);
    RUN_TEST(test_rtsp_connect_backoff_uses_time_after_attempt);
    RUN_TEST(test_rtsp_short_write_does_not_commit_rtp_cursor);
    RUN_TEST(test_rtp_cursor_ignores_opus_clock_reset);
    RUN_TEST(test_audio_lifecycle_restart_orders_pause_stop_start_resume);
    RUN_TEST(test_audio_lifecycle_failed_start_keeps_consumers_paused);
    RUN_TEST(test_ntp_sync_requires_callback_not_stale_epoch);
    RUN_TEST(test_wifi_recovery_uses_backoff_before_reconnect);

    RUN_TEST(test_netconfig_defaults_audio_system);
    RUN_TEST(test_netconfig_schema_version_initial);
    RUN_TEST(test_netconfig_migration_v0_to_v1);
    RUN_TEST(test_netconfig_migration_v1_noop);
    RUN_TEST(test_netconfig_migration_v1_defaults);
    RUN_TEST(test_netconfig_load_save_roundtrip);
    RUN_TEST(test_netconfig_validation_bounds);
    RUN_TEST(test_command_auth_payload_legacy_and_nonce);
    RUN_TEST(test_command_auth_expired_nonce);
    RUN_TEST(test_command_auth_hmac_roundtrip);
    RUN_TEST(test_command_auth_verify_hmac);

    // ---------------------------------------------------------------------------
    // MQTTManager — nonce cache (host-testable via testOnly)
    // ---------------------------------------------------------------------------
    RUN_TEST(test_mqtt_nonce_cache_rejects_duplicate);
    RUN_TEST(test_mqtt_nonce_expires_after_ttl);
    RUN_TEST(test_mqtt_nonce_cache_overflow_is_fail_closed);

    // ---------------------------------------------------------------------------
    // ValidateUtil — URL and hostname validation
    // ---------------------------------------------------------------------------
    RUN_TEST(test_validate_url_https_ok);
    RUN_TEST(test_validate_hostname_ok);

    RUN_TEST(test_netconfig_soft_apply_no_reboot);
    RUN_TEST(test_netconfig_soft_apply_reboot_required);
    RUN_TEST(test_netconfig_soft_apply_rtsp_reboot);
    RUN_TEST(test_netconfig_soft_apply_ntp_no_reboot);
    RUN_TEST(test_netconfig_write_error_simulation);
    RUN_TEST(test_netconfig_factory_reset);
    RUN_TEST(test_netconfig_network_reset_preserves_audio);
    RUN_TEST(test_netconfig_to_json_roundtrip);
    RUN_TEST(test_netconfig_hmac_key_roundtrip);
    RUN_TEST(test_netconfig_led_mode_default);
    return UNITY_END();
}

// ──────────────────────────────────────────────────
// Mel atmospheric attenuation (optional MEL EQ hook)
// ──────────────────────────────────────────────────

void test_mel_iso9613_natlog_apply(void) {
    MelSpectrogram mel;
    TEST_ASSERT_TRUE(mel.begin());
    int16_t pcm[MelSpectrogram::kWindowLength];
    for (int i = 0; i < MelSpectrogram::kWindowLength; i++) {
        float t = (float)i / (float)MelSpectrogram::kWindowLength;
        pcm[i] = (int16_t)(8000.0f * sinf(2.0f * 3.14159265f * 1000.0f * t));
    }
    float baseline[MelSpectrogram::kNumBands];
    mel.computeFrame(pcm, baseline);

    float att[MelSpectrogram::kNumBands];
    for (int b = 0; b < MelSpectrogram::kNumBands; ++b) att[b] = 10.0f;  // 10 dB
    mel.setAtmosphericAttenuationDb(att, true);
    float corrected[MelSpectrogram::kNumBands];
    mel.computeFrame(pcm, corrected);

    constexpr float kDbToNat = 0.230258509f;
    for (int b = 0; b < MelSpectrogram::kNumBands; ++b) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, baseline[b] - 10.0f * kDbToNat, corrected[b]);
    }

    mel.setAtmosphericAttenuationDb(nullptr, false);
    float off[MelSpectrogram::kNumBands];
    mel.computeFrame(pcm, off);
    for (int b = 0; b < MelSpectrogram::kNumBands; ++b) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, baseline[b], off[b]);
    }
}


// ──────────────────────────────────────────────────
// Sensor intelligence Phase 1
// ──────────────────────────────────────────────────


void test_dsp_level_compensate_agc_off_ignores_chip_gain() {
    DspLevelCompParams p{};
    p.agcGainDb = 30.0f;
    p.agcEnabled = false;
    p.micGainLin = 10.0f;  // +20 dB
    p.asroutGainLin = 1.0f;
    p.asroutEnabled = true;
    p.attnsMode = 0;
    p.softwareGainLin = 1.0f;
    float g = dspEffectiveGainDb(p);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 20.0f, g);
}
