/**
 * @file    NetConfig.cpp
 * @brief   NVS: загрузка/сохранение, версия схемы, миграция, soft apply.
 *
 * Namespace «rtspmic»; K_* — короткие ключи (лимит NVS ~15 символов).
 * schema_ver (KEY_SCHEMA_VER) — bump при NVS_SCHEMA_VERSION для миграции.
 */
#include "NetConfig.h"
#include <Preferences.h>
#include <cmath>
#include <string.h>

static const char *K_MQTT_EN     = "mqtt_en";
static const char *K_MQTT_HOST   = "mqtt_host";
static const char *K_MQTT_PORT   = "mqtt_port";
static const char *K_MQTT_USER   = "mqtt_user";
static const char *K_MQTT_PASS   = "mqtt_pass";
static const char *K_MQTT_HA     = "mqtt_ha";
static const char *K_RTSP_EN     = "rtsp_en";
static const char *K_RTSP_HOST   = "rtsp_host";
static const char *K_RTSP_PORT   = "rtsp_port";
static const char *K_AUD_SETUP   = "aud_setup";
static const char *K_NTP_HOST    = "ntp_host";
    static const char *K_HPF_EN       = "hpf_en";
    static const char *K_HPF_CUTOFF   = "hpf_cutoff";
    static const char *K_HPF_MODE     = "hpf_mode";
static const char *K_TIMEZONE     = "timezone";
static const char *K_SCHED_RESET  = "sched_reset";
static const char *K_SCHED_RESET_HH = "sched_reset_hh";
static const char *K_SCHED_RESET_MM = "sched_reset_mm";
static const char *K_CAL_OFFSET   = "cal_offset";
static const char *K_HMAC_KEY     = "hmac_key";
static const char *K_SEC_LED      = "sec_led_mode";
static const char *K_DSP_AGC      = "dsp_agc_en";
static const char *K_DSP_NS_STAT  = "dsp_ns_stat";
static const char *K_DSP_NS_NSTAT = "dsp_ns_nstat";
static const char *K_DSP_AGCMAX   = "dsp_agc_max";
static const char *K_DSP_AGCDES   = "dsp_agc_des";
static const char *K_DSP_LIM_EN   = "dsp_lim_en";
static const char *K_DSP_LIM_THR  = "dsp_lim_thr";
static const char *K_DSP_AGC_TIME = "dsp_agc_time";
static const char *K_DSP_AGC_FT   = "dsp_agc_ftime";
static const char *K_DSP_AGC_ASLOW= "dsp_agc_aslow";
static const char *K_DSP_AGC_AFAST= "dsp_agc_afast";
static const char *K_DSP_MIC_GAIN = "dsp_mic_gain";
static const char *K_AEC_ENV      = "aec_env";
static const char *K_ASROUT       = "asrout_en";
static const char *K_ASROUT_GAIN  = "asrout_gain";
static const char *K_ECHO_EN      = "echo_en";
static const char *K_LOUDSPEAKER_EN = "loudspeaker_en";
static const char *K_FIXED_BEAM   = "fixed_beam";
static const char *K_FIXED_AZ0    = "fixed_az0";
static const char *K_FIXED_AZ1    = "fixed_az1";
static const char *K_FIXED_GATE   = "fixed_gate";
static const char *K_ATTNS_MODE   = "attns_mode";
static const char *K_ATTNS_NOM    = "attns_nom";
static const char *K_ATTNS_SLOPE  = "attns_slope";
static const char *K_REF_GAIN     = "ref_gain";
static const char *K_SYS_DELAY    = "sys_delay";

const char *NetConfig::NVS_NS        = "rtspmic";
const uint8_t NetConfig::SCHEMA_VERSION = NVS_SCHEMA_VERSION;
const char *NetConfig::KEY_SCHEMA_VER  = "schema_ver";

static constexpr float  NCFG_DEFAULT_HPF_CUTOFF        = 0.0f;
static constexpr float  NCFG_DEFAULT_CAL_OFFSET        = 0.0f;
static constexpr bool   NCFG_DEFAULT_HPF_ENABLED       = false;
static constexpr uint8_t NCFG_DEFAULT_HPF_MODE         = 0;  // Off (XMOS 0..4)
static constexpr bool   NCFG_DEFAULT_SCHED_RESET       = false;
static constexpr bool   NCFG_DEFAULT_DSP_AGC            = true;  // нужно: без AGC микрофон тихий
static constexpr float  NCFG_DEFAULT_DSP_NS_STAT        = 1.00f; // 1.0=NS off (XMOS); 0=max suppress
static constexpr float  NCFG_DEFAULT_DSP_NS_NONSTAT     = 1.00f; // 1.0=NN off; <0.5 жёстче
static constexpr float  NCFG_DEFAULT_DSP_MIC_GAIN       = 8.0f;   // было 25: AGC+25 → mel≈+9, UI ломался
static constexpr uint8_t NCFG_DEFAULT_AEC_ENV           = 1;  // AEC_ENV_NOISY
static constexpr uint8_t NCFG_DEFAULT_ASROUT            = 1;  // beams/ASR; residuals — опция под монитор
static constexpr float   NCFG_DEFAULT_ASROUT_GAIN       = 1.0f;
static constexpr bool    NCFG_DEFAULT_ECHO_EN           = false;  // OFF: PP echo портит спектр; ON только с сиреной
static constexpr uint8_t NCFG_DEFAULT_FIXED_BEAM        = 0;
static constexpr float   NCFG_DEFAULT_FIXED_AZ0         = 0.0f;
static constexpr float   NCFG_DEFAULT_FIXED_AZ1         = 180.0f;
static constexpr uint8_t NCFG_DEFAULT_FIXED_GATE        = 0;
static constexpr uint8_t NCFG_DEFAULT_ATTNS_MODE        = 0;  // OFF: ATTNS даёт «дыхание» AGC в тишине — плохо для RTSP
static constexpr float   NCFG_DEFAULT_ATTNS_NOM         = 1.0f;
static constexpr float   NCFG_DEFAULT_ATTNS_SLOPE       = 0.2f;
static constexpr bool    NCFG_DEFAULT_LOUDSPEAKER_EN    = false;
static constexpr float   NCFG_DEFAULT_REF_GAIN          = 1.5f;
static constexpr int32_t NCFG_DEFAULT_SYS_DELAY         = 0;
static constexpr float  NCFG_DEFAULT_DSP_AGC_MAXGAIN    = 5.0f;   // низкий потолок: меньше насыщения MEL в тишине
static constexpr float  NCFG_DEFAULT_DSP_AGC_DESIRED     = 0.015f; // уверенный уровень
static constexpr bool   NCFG_DEFAULT_DSP_LIMITER        = true;
static constexpr float  NCFG_DEFAULT_DSP_LIMIT_THR      = 0.47f;
static constexpr float  NCFG_DEFAULT_DSP_AGC_TIME       = 3.0f;   // плавно
static constexpr float  NCFG_DEFAULT_DSP_AGC_FASTTIME   = 0.5f;
static constexpr float  NCFG_DEFAULT_DSP_AGC_ALPHA_SLOW = 0.99f;
static constexpr float  NCFG_DEFAULT_DSP_AGC_ALPHA_FAST = 0.15f;

uint8_t NetConfig::getStoredVersion() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return 0;
    uint8_t ver = prefs.getUChar(KEY_SCHEMA_VER, 0);
    prefs.end();
    return ver;
}

bool NetConfig::runMigration() {
    uint8_t stored = getStoredVersion();
    if (stored >= SCHEMA_VERSION) return true;
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = prefs.putUChar(KEY_SCHEMA_VER, SCHEMA_VERSION);
    prefs.end();
    return ok;
}

void NetConfig::applyDefaults(NetConfigData &out) {
    memset(&out, 0, sizeof(out));
    out.mqttEnabled = false;
    strncpy(out.mqttHost, "192.168.1.100", sizeof(out.mqttHost) - 1);
    out.mqttPort = 1883;
    out.mqttHaDiscovery = false;
    out.rtspRemoteEnabled = false;
    strncpy(out.rtspRemoteHost, "192.168.1.200", sizeof(out.rtspRemoteHost) - 1);
    out.rtspRemotePort = 554;
    strncpy(out.ntpHost, NTP_DEFAULT_SERVER, sizeof(out.ntpHost) - 1);
    out.hpfMode      = NCFG_DEFAULT_HPF_MODE;
    out.hpfEnabled   = NCFG_DEFAULT_HPF_ENABLED;
    out.hpfCutoffHz  = NCFG_DEFAULT_HPF_CUTOFF;
    out.audioSetupMode = false;
    out.timezone[0]       = '\0';
    out.scheduledReset    = NCFG_DEFAULT_SCHED_RESET;
    out.scheduledResetHour   = 3;
    out.scheduledResetMinute = 0;
    out.calibrationOffsetDb = NCFG_DEFAULT_CAL_OFFSET;
    memset(&out.hmacKey, 0, sizeof(out.hmacKey));
    out.hmacKey.valid = false;
    out.security.ledMode = static_cast<uint8_t>(LED_MODE_STATUS);
    out.dspAgcEnabled = NCFG_DEFAULT_DSP_AGC;
    out.dspNsStationary = NCFG_DEFAULT_DSP_NS_STAT;
    out.dspNsNonStationary = NCFG_DEFAULT_DSP_NS_NONSTAT;
    out.dspMicGain = NCFG_DEFAULT_DSP_MIC_GAIN;
    out.aecEnvMode = NCFG_DEFAULT_AEC_ENV;
    out.loudspeakerPresent = NCFG_DEFAULT_LOUDSPEAKER_EN;
    out.asroutEnabled = NCFG_DEFAULT_ASROUT;
    out.asroutGain = NCFG_DEFAULT_ASROUT_GAIN;
    out.echoSuppressionEnabled = NCFG_DEFAULT_ECHO_EN;
    out.fixedBeamsEnabled = NCFG_DEFAULT_FIXED_BEAM;
    out.fixedBeamAzDeg[0] = NCFG_DEFAULT_FIXED_AZ0;
    out.fixedBeamAzDeg[1] = NCFG_DEFAULT_FIXED_AZ1;
    out.fixedBeamGating = NCFG_DEFAULT_FIXED_GATE;
    out.attnsMode = NCFG_DEFAULT_ATTNS_MODE;
    out.attnsNominal = NCFG_DEFAULT_ATTNS_NOM;
    out.attnsSlope = NCFG_DEFAULT_ATTNS_SLOPE;
    out.refGain = NCFG_DEFAULT_REF_GAIN;
    out.sysDelaySamples = NCFG_DEFAULT_SYS_DELAY;
    out.dspAgcMaxGain = NCFG_DEFAULT_DSP_AGC_MAXGAIN;
    out.dspAgcDesiredLevel = NCFG_DEFAULT_DSP_AGC_DESIRED;
    out.dspLimiterEnabled = NCFG_DEFAULT_DSP_LIMITER;
    out.dspLimiterThreshold = NCFG_DEFAULT_DSP_LIMIT_THR;
    out.dspAgcTime = NCFG_DEFAULT_DSP_AGC_TIME;
    out.dspAgcFastTime = NCFG_DEFAULT_DSP_AGC_FASTTIME;
    out.dspAgcAlphaSlow = NCFG_DEFAULT_DSP_AGC_ALPHA_SLOW;
    out.dspAgcAlphaFast = NCFG_DEFAULT_DSP_AGC_ALPHA_FAST;
}

void NetConfig::load(NetConfigData &out) {
    applyDefaults(out);
    if (!runMigration()) {
        Serial.printf("[NVS] migration failed — loading best-effort\n");
    }
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return;
    out.mqttEnabled = prefs.getBool(K_MQTT_EN, out.mqttEnabled);
    String host = prefs.getString(K_MQTT_HOST, out.mqttHost);
    if (host.length() > 0) strncpy(out.mqttHost, host.c_str(), sizeof(out.mqttHost) - 1);
    out.mqttPort = prefs.getUShort(K_MQTT_PORT, out.mqttPort);
    String user = prefs.getString(K_MQTT_USER, "");
    strncpy(out.mqttUser, user.c_str(), sizeof(out.mqttUser) - 1);
    String pass = prefs.getString(K_MQTT_PASS, "");
    strncpy(out.mqttPass, pass.c_str(), sizeof(out.mqttPass) - 1);
    out.mqttHaDiscovery = prefs.getBool(K_MQTT_HA, out.mqttHaDiscovery);
    out.rtspRemoteEnabled = prefs.getBool(K_RTSP_EN, out.rtspRemoteEnabled);
    String rh = prefs.getString(K_RTSP_HOST, out.rtspRemoteHost);
    if (rh.length() > 0) strncpy(out.rtspRemoteHost, rh.c_str(), sizeof(out.rtspRemoteHost) - 1);
    out.rtspRemotePort = prefs.getUShort(K_RTSP_PORT, out.rtspRemotePort);
    out.audioSetupMode = prefs.getBool(K_AUD_SETUP, out.audioSetupMode);
    String ntp = prefs.getString(K_NTP_HOST, out.ntpHost);
    if (ntp.length() > 0) strncpy(out.ntpHost, ntp.c_str(), sizeof(out.ntpHost) - 1);
    // hpf_mode (0..4) preferred; legacy hpf_en → 2/0.
    {
        uint8_t mode = prefs.getUChar(K_HPF_MODE, 255);
        if (mode > 4) {
            out.hpfEnabled = prefs.getBool(K_HPF_EN, out.hpfEnabled);
            mode = out.hpfEnabled ? 2 : 0;
        }
        out.hpfMode = mode;
        syncHpfFields(out);
    }
    String tz = prefs.getString(K_TIMEZONE, "");
    if (tz.length() > 0) strncpy(out.timezone, tz.c_str(), sizeof(out.timezone) - 1);
    out.scheduledReset    = prefs.getBool(K_SCHED_RESET, out.scheduledReset);
    out.scheduledResetHour   = prefs.getUChar(K_SCHED_RESET_HH, out.scheduledResetHour);
    out.scheduledResetMinute = prefs.getUChar(K_SCHED_RESET_MM, out.scheduledResetMinute);
    if (out.scheduledResetHour > 23) out.scheduledResetHour = 3;
    if (out.scheduledResetMinute > 59) out.scheduledResetMinute = 0;
    out.calibrationOffsetDb = prefs.getFloat(K_CAL_OFFSET, out.calibrationOffsetDb);
    String hmacHex = prefs.getString(K_HMAC_KEY, "");
    memset(out.hmacKey.bytes, 0, sizeof(out.hmacKey.bytes));
    out.hmacKey.valid = false;
    if (hmacHex.length() == 64) {
        bool ok = true;
        for (int i = 0; i < 32; i++) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            const int hi = hexVal(hmacHex[i * 2]);
            const int lo = hexVal(hmacHex[i * 2 + 1]);
            if (hi < 0 || lo < 0) { ok = false; break; }
            out.hmacKey.bytes[i] = (uint8_t)((hi << 4) | lo);
        }
        out.hmacKey.valid = ok;
        if (!ok) memset(out.hmacKey.bytes, 0, sizeof(out.hmacKey.bytes));
    }
    out.security.ledMode = prefs.getUChar(K_SEC_LED, out.security.ledMode);
    out.dspAgcEnabled = prefs.getBool(K_DSP_AGC, out.dspAgcEnabled);
    out.dspNsStationary = prefs.getFloat(K_DSP_NS_STAT, out.dspNsStationary);
    out.dspNsNonStationary = prefs.getFloat(K_DSP_NS_NSTAT, out.dspNsNonStationary);
    out.dspAgcMaxGain = prefs.getFloat(K_DSP_AGCMAX, out.dspAgcMaxGain);
    out.dspAgcDesiredLevel = prefs.getFloat(K_DSP_AGCDES, out.dspAgcDesiredLevel);
    out.dspLimiterEnabled = prefs.getBool(K_DSP_LIM_EN, out.dspLimiterEnabled);
    out.dspLimiterThreshold = prefs.getFloat(K_DSP_LIM_THR, out.dspLimiterThreshold);
    out.dspAgcTime = prefs.getFloat(K_DSP_AGC_TIME, out.dspAgcTime);
    out.dspAgcFastTime = prefs.getFloat(K_DSP_AGC_FT, out.dspAgcFastTime);
    out.dspAgcAlphaSlow = prefs.getFloat(K_DSP_AGC_ASLOW, out.dspAgcAlphaSlow);
    out.dspAgcAlphaFast = prefs.getFloat(K_DSP_AGC_AFAST, out.dspAgcAlphaFast);
    out.dspMicGain = prefs.getFloat(K_DSP_MIC_GAIN, out.dspMicGain);
    out.aecEnvMode = prefs.getUChar(K_AEC_ENV, out.aecEnvMode);
    out.loudspeakerPresent = prefs.getBool(K_LOUDSPEAKER_EN, out.loudspeakerPresent);
    out.asroutEnabled = prefs.getUChar(K_ASROUT, out.asroutEnabled);
    out.asroutGain = prefs.getFloat(K_ASROUT_GAIN, out.asroutGain);
    out.echoSuppressionEnabled = prefs.getBool(K_ECHO_EN, out.echoSuppressionEnabled);
    out.fixedBeamsEnabled = prefs.getUChar(K_FIXED_BEAM, out.fixedBeamsEnabled);
    out.fixedBeamAzDeg[0] = prefs.getFloat(K_FIXED_AZ0, out.fixedBeamAzDeg[0]);
    out.fixedBeamAzDeg[1] = prefs.getFloat(K_FIXED_AZ1, out.fixedBeamAzDeg[1]);
    out.fixedBeamGating = prefs.getUChar(K_FIXED_GATE, out.fixedBeamGating);
    out.attnsMode = prefs.getUChar(K_ATTNS_MODE, out.attnsMode);
    out.attnsNominal = prefs.getFloat(K_ATTNS_NOM, out.attnsNominal);
    out.attnsSlope = prefs.getFloat(K_ATTNS_SLOPE, out.attnsSlope);
    out.refGain = prefs.getFloat(K_REF_GAIN, out.refGain);
    out.sysDelaySamples = prefs.getInt(K_SYS_DELAY, out.sysDelaySamples);
    prefs.end();
    sanitizeLoudspeakerOff(out);
}

bool NetConfig::save(const NetConfigData &in) {
    NetConfigData cfg = in;
    syncHpfFields(cfg);
    sanitizeLoudspeakerOff(cfg);
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
#ifdef UNIT_TEST
    // Stub: simulate full NVS write failure without relying on put*()==0
    // (on device put*()==0 also means ESP_ERR_NVS_CONTENT_SAME).
    extern bool g_prefs_write_fail;
    if (g_prefs_write_fail) { prefs.end(); return false; }
#endif
    // ESP32 Preferences: put*() часто возвращает 0 при записи ТОГО ЖЕ значения
    // (NVS CONTENT_SAME) — раньше это ломало /api/audio с nvs_error.
    prefs.putUChar(KEY_SCHEMA_VER, SCHEMA_VERSION);
    prefs.putBool(K_MQTT_EN, in.mqttEnabled);
    prefs.putString(K_MQTT_HOST, in.mqttHost);
    prefs.putUShort(K_MQTT_PORT, in.mqttPort);
    prefs.putString(K_MQTT_USER, in.mqttUser);
    prefs.putString(K_MQTT_PASS, in.mqttPass);
    prefs.putBool(K_MQTT_HA, in.mqttHaDiscovery);
    prefs.putBool(K_RTSP_EN, in.rtspRemoteEnabled);
    prefs.putString(K_RTSP_HOST, in.rtspRemoteHost);
    prefs.putUShort(K_RTSP_PORT, in.rtspRemotePort);
    prefs.putBool(K_AUD_SETUP, in.audioSetupMode);
    prefs.putString(K_NTP_HOST, in.ntpHost);
    prefs.putUChar(K_HPF_MODE, cfg.hpfMode);
    prefs.putBool(K_HPF_EN, cfg.hpfEnabled);
    prefs.putFloat(K_HPF_CUTOFF, cfg.hpfCutoffHz);
    prefs.putString(K_TIMEZONE, in.timezone);
    prefs.putBool(K_SCHED_RESET, in.scheduledReset);
    prefs.putUChar(K_SCHED_RESET_HH, in.scheduledResetHour);
    prefs.putUChar(K_SCHED_RESET_MM, in.scheduledResetMinute);
    prefs.putFloat(K_CAL_OFFSET, in.calibrationOffsetDb);
    {
        char hmacHex[65];
        if (in.hmacKey.valid) {
            for (int i = 0; i < 32 && i < 64; i++) snprintf(hmacHex + i * 2, 3, "%02X", in.hmacKey.bytes[i]);
            hmacHex[64] = '\0';
        } else { hmacHex[0] = '\0'; }
        prefs.putString(K_HMAC_KEY, hmacHex);
    }
    prefs.putUChar(K_SEC_LED, in.security.ledMode);
    prefs.putBool(K_DSP_AGC, in.dspAgcEnabled);
    prefs.putFloat(K_DSP_NS_STAT, in.dspNsStationary);
    prefs.putFloat(K_DSP_NS_NSTAT, in.dspNsNonStationary);
    prefs.putFloat(K_DSP_AGCMAX, in.dspAgcMaxGain);
    prefs.putFloat(K_DSP_AGCDES, in.dspAgcDesiredLevel);
    prefs.putBool(K_DSP_LIM_EN, in.dspLimiterEnabled);
    prefs.putFloat(K_DSP_LIM_THR, in.dspLimiterThreshold);
    prefs.putFloat(K_DSP_AGC_TIME, in.dspAgcTime);
    prefs.putFloat(K_DSP_AGC_FT, in.dspAgcFastTime);
    prefs.putFloat(K_DSP_AGC_ASLOW, in.dspAgcAlphaSlow);
    prefs.putFloat(K_DSP_AGC_AFAST, in.dspAgcAlphaFast);
    prefs.putFloat(K_DSP_MIC_GAIN, in.dspMicGain);
    prefs.putUChar(K_AEC_ENV, cfg.aecEnvMode);
    prefs.putBool(K_LOUDSPEAKER_EN, cfg.loudspeakerPresent);
    prefs.putUChar(K_ASROUT, cfg.asroutEnabled);
    prefs.putFloat(K_ASROUT_GAIN, cfg.asroutGain);
    prefs.putBool(K_ECHO_EN, cfg.echoSuppressionEnabled);
    prefs.putUChar(K_FIXED_BEAM, in.fixedBeamsEnabled);
    prefs.putFloat(K_FIXED_AZ0, in.fixedBeamAzDeg[0]);
    prefs.putFloat(K_FIXED_AZ1, in.fixedBeamAzDeg[1]);
    prefs.putUChar(K_FIXED_GATE, in.fixedBeamGating);
    prefs.putUChar(K_ATTNS_MODE, in.attnsMode);
    prefs.putFloat(K_ATTNS_NOM, in.attnsNominal);
    prefs.putFloat(K_ATTNS_SLOPE, in.attnsSlope);
    prefs.putFloat(K_REF_GAIN, in.refGain);
    prefs.putInt(K_SYS_DELAY, in.sysDelaySamples);
    prefs.end();
    return true;
}

SoftApplyResult NetConfig::softApply(const NetConfigData &cfg,
                                     bool mqttChanged,
                                     bool rtspChanged, bool ntpChanged,
                                     bool audioChanged, bool systemChanged,
                                     bool securityChanged)
{
    SoftApplyResult result;
    result.success = false;
    result.rebootRecommended = false;
    result.errorMsg = nullptr;
    if (!save(cfg)) { result.errorMsg = "nvs_write_failed"; return result; }
    result.success = true;
    result.rebootRecommended = mqttChanged || rtspChanged || ntpChanged;
    return result;
}

bool NetConfig::factoryReset() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = prefs.clear();
    prefs.end();
    return ok;
}

bool NetConfig::networkReset() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    prefs.remove(K_MQTT_EN);
    prefs.remove(K_MQTT_HOST);
    prefs.remove(K_MQTT_PORT);
    prefs.remove(K_MQTT_USER);
    prefs.remove(K_MQTT_PASS);
    prefs.remove(K_MQTT_HA);
    prefs.remove(K_RTSP_EN);
    prefs.remove(K_RTSP_HOST);
    prefs.remove(K_RTSP_PORT);
    prefs.remove(K_AUD_SETUP);
    prefs.remove(K_NTP_HOST);
    prefs.end();
    return true;
}

void NetConfig::toJson(JsonObject obj, const NetConfigData &cfg, bool maskSecrets) {
    obj["mqtt_enabled"] = cfg.mqttEnabled;
    obj["mqtt_host"] = cfg.mqttHost;
    obj["mqtt_port"] = cfg.mqttPort;
    obj["mqtt_user"] = cfg.mqttUser;
    if (maskSecrets && cfg.mqttPass[0]) { obj["mqtt_pass"] = "********"; } else { obj["mqtt_pass"] = cfg.mqttPass; }
    obj["mqtt_ha_discovery"] = cfg.mqttHaDiscovery;
    obj["rtsp_remote_enabled"] = cfg.rtspRemoteEnabled;
    obj["rtsp_remote_host"] = cfg.rtspRemoteHost;
    obj["rtsp_remote_port"] = cfg.rtspRemotePort;
    obj["ntp_host"] = cfg.ntpHost;
    obj["hpf_mode"] = cfg.hpfMode;
    obj["hpf_enabled"] = cfg.hpfEnabled;
    obj["hpf_cutoff_hz"] = cfg.hpfCutoffHz;
    obj["audio_setup_mode"] = cfg.audioSetupMode;
    obj["timezone"] = cfg.timezone;
    obj["scheduled_reset"] = cfg.scheduledReset;
    obj["sched_reset_hh"] = cfg.scheduledResetHour;
    obj["sched_reset_mm"] = cfg.scheduledResetMinute;
    obj["calibration_offset_db"] = cfg.calibrationOffsetDb;
    if (maskSecrets && cfg.hmacKey.valid) { obj["hmac_key"] = "********"; } else { char h[65]={}; if(cfg.hmacKey.valid){for(int i=0;i<32;i++)snprintf(h+i*2,3,"%02X",cfg.hmacKey.bytes[i]);} obj["hmac_key"]=h; }
    obj["led_mode"] = cfg.security.ledMode;
    obj["dsp_agc_enabled"] = cfg.dspAgcEnabled;
    obj["dsp_ns_stat"] = cfg.dspNsStationary;
    obj["dsp_ns_nstat"] = cfg.dspNsNonStationary;
    obj["dsp_agc_max_gain"] = cfg.dspAgcMaxGain;
    obj["dsp_agc_des_level"] = cfg.dspAgcDesiredLevel;
    obj["dsp_limiter_enabled"] = cfg.dspLimiterEnabled;
    obj["dsp_limiter_threshold"] = cfg.dspLimiterThreshold;
    obj["dsp_agc_time"] = cfg.dspAgcTime;
    obj["dsp_agc_fasttime"] = cfg.dspAgcFastTime;
    obj["dsp_agc_alpha_slow"] = cfg.dspAgcAlphaSlow;
    obj["dsp_agc_alpha_fast"] = cfg.dspAgcAlphaFast;
    obj["dsp_mic_gain"] = cfg.dspMicGain;
    obj["aec_env"] = cfg.aecEnvMode;
    obj["loudspeaker_present"] = cfg.loudspeakerPresent;
    obj["asrout"] = cfg.asroutEnabled;
    obj["asrout_gain"] = cfg.asroutGain;
    obj["echo_suppression"] = cfg.echoSuppressionEnabled;
    // Fixed beams: Adaptive default; keys kept for API compatibility.
    obj["fixed_beams"] = 0;
    obj["fixed_beam_az0"] = cfg.fixedBeamAzDeg[0];
    obj["fixed_beam_az1"] = cfg.fixedBeamAzDeg[1];
    obj["fixed_beam_gating"] = 0;
    obj["attns_mode"] = cfg.attnsMode;
    obj["attns_nominal"] = cfg.attnsNominal;
    obj["attns_slope"] = cfg.attnsSlope;
    obj["ref_gain"] = cfg.refGain;
    obj["sys_delay"] = cfg.sysDelaySamples;
}

bool NetConfig::fromJson(JsonVariantConst doc, NetConfigData &cfg) {
    auto applyPort = [](JsonVariantConst v, uint16_t &dst) {
        if (!v.is<int>()) return;
        const int p = v.as<int>();
        if (p >= 1 && p <= 65535) dst = (uint16_t)p;
    };
    if (doc["mqtt_enabled"].is<bool>()) cfg.mqttEnabled = doc["mqtt_enabled"];
    if (doc["mqtt_host"].is<const char *>()) strncpy(cfg.mqttHost, doc["mqtt_host"], sizeof(cfg.mqttHost)-1);
    applyPort(doc["mqtt_port"], cfg.mqttPort);
    if (doc["mqtt_user"].is<const char *>()) strncpy(cfg.mqttUser, doc["mqtt_user"], sizeof(cfg.mqttUser)-1);
    if (doc["mqtt_pass"].is<const char *>() && strcmp(doc["mqtt_pass"], "********")!=0) strncpy(cfg.mqttPass, doc["mqtt_pass"], sizeof(cfg.mqttPass)-1);
    if (doc["mqtt_ha_discovery"].is<bool>()) cfg.mqttHaDiscovery = doc["mqtt_ha_discovery"];
    if (doc["rtsp_remote_enabled"].is<bool>()) cfg.rtspRemoteEnabled = doc["rtsp_remote_enabled"];
    if (doc["rtsp_remote_host"].is<const char *>()) strncpy(cfg.rtspRemoteHost, doc["rtsp_remote_host"], sizeof(cfg.rtspRemoteHost)-1);
    applyPort(doc["rtsp_remote_port"], cfg.rtspRemotePort);
    if (doc["ntp_host"].is<const char *>() && doc["ntp_host"].as<const char*>()[0]) strncpy(cfg.ntpHost, doc["ntp_host"], sizeof(cfg.ntpHost)-1);
    if (!doc["hpf_mode"].isNull()) {
        int m = doc["hpf_mode"].as<int>();
        if (m >= 0 && m <= 4) cfg.hpfMode = (uint8_t)m;
    } else if (doc["hpf_enabled"].is<bool>()) {
        cfg.hpfMode = doc["hpf_enabled"].as<bool>() ? 2 : 0;
    }
    syncHpfFields(cfg);
    if (doc["audio_setup_mode"].is<bool>()) cfg.audioSetupMode = doc["audio_setup_mode"];
    if (doc["timezone"].is<const char *>()) strncpy(cfg.timezone, doc["timezone"], sizeof(cfg.timezone)-1);
    if (doc["scheduled_reset"].is<bool>()) cfg.scheduledReset = doc["scheduled_reset"];
    if (doc["sched_reset_hh"].is<int>()) { int v=doc["sched_reset_hh"].as<int>(); if(v>=0&&v<=23)cfg.scheduledResetHour=(uint8_t)v; }
    if (doc["sched_reset_mm"].is<int>()) { int v=doc["sched_reset_mm"].as<int>(); if(v>=0&&v<=59)cfg.scheduledResetMinute=(uint8_t)v; }
    if (!doc["calibration_offset_db"].isNull()) { float c=doc["calibration_offset_db"].as<float>(); if(c>=-40.f&&c<=40.f)cfg.calibrationOffsetDb=c; }
    if (doc["hmac_key"].is<const char *>()) {
        const char *hk = doc["hmac_key"];
        if (hk && strcmp(hk, "********") != 0) {
            size_t hl = strlen(hk);
            if (hl == 64) {
                bool ok = true;
                for (int i = 0; i < 32; i++) {
                    auto h = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return -1;
                    };
                    const int hi = h(hk[i * 2]);
                    const int lo = h(hk[i * 2 + 1]);
                    if (hi < 0 || lo < 0) { ok = false; break; }
                    cfg.hmacKey.bytes[i] = (uint8_t)((hi << 4) | lo);
                }
                if (ok) cfg.hmacKey.valid = true;
                else { memset(&cfg.hmacKey, 0, sizeof(cfg.hmacKey)); cfg.hmacKey.valid = false; }
            } else if (hl == 0) {
                memset(&cfg.hmacKey, 0, sizeof(cfg.hmacKey));
                cfg.hmacKey.valid = false;
            }
        }
    }
    if (doc["led_mode"].is<int>()) { int lm=doc["led_mode"].as<int>(); if(lm>=0&&lm<=2)cfg.security.ledMode=(uint8_t)lm; }
    if (doc["dsp_agc_enabled"].is<bool>()) cfg.dspAgcEnabled = doc["dsp_agc_enabled"];
    if (!doc["dsp_ns_stat"].isNull()) { float v=doc["dsp_ns_stat"].as<float>(); if(v>=0&&v<=1)cfg.dspNsStationary=v; }
    if (!doc["dsp_ns_nstat"].isNull()) { float v=doc["dsp_ns_nstat"].as<float>(); if(v>=0&&v<=1)cfg.dspNsNonStationary=v; }
    if (!doc["dsp_agc_max_gain"].isNull()) { float v=doc["dsp_agc_max_gain"].as<float>(); if(v>=1&&v<=1000)cfg.dspAgcMaxGain=v; }
    if (!doc["dsp_agc_des_level"].isNull()) { float v=doc["dsp_agc_des_level"].as<float>(); if(v>=1e-8f&&v<=1)cfg.dspAgcDesiredLevel=v; }
    if (!doc["dsp_limiter_enabled"].isNull() && doc["dsp_limiter_enabled"].is<bool>()) cfg.dspLimiterEnabled = doc["dsp_limiter_enabled"];
    if (!doc["dsp_limiter_threshold"].isNull()) { float v=doc["dsp_limiter_threshold"].as<float>(); if(v>=1e-8f&&v<=1)cfg.dspLimiterThreshold=v; }
    if (!doc["dsp_agc_time"].isNull()) { float v=doc["dsp_agc_time"].as<float>(); if(v>=0.5f&&v<=4)cfg.dspAgcTime=v; }
    if (!doc["dsp_agc_fasttime"].isNull()) { float v=doc["dsp_agc_fasttime"].as<float>(); if(v>=0.05f&&v<=4)cfg.dspAgcFastTime=v; }
    if (!doc["dsp_agc_alpha_slow"].isNull()) { float v=doc["dsp_agc_alpha_slow"].as<float>(); if(v>=0&&v<=1)cfg.dspAgcAlphaSlow=v; }
    if (!doc["dsp_agc_alpha_fast"].isNull()) { float v=doc["dsp_agc_alpha_fast"].as<float>(); if(v>=0&&v<=1)cfg.dspAgcAlphaFast=v; }
    if (!doc["dsp_mic_gain"].isNull()) { float v=doc["dsp_mic_gain"].as<float>(); if(v>=0.1f&&v<=1000)cfg.dspMicGain=v; }
    if (!doc["aec_env"].isNull()) { int v=doc["aec_env"].as<int>(); if(v==0||v==1)cfg.aecEnvMode=(uint8_t)v; }
    if (!doc["loudspeaker_present"].isNull()) {
        if (doc["loudspeaker_present"].is<bool>()) cfg.loudspeakerPresent = doc["loudspeaker_present"].as<bool>();
        else cfg.loudspeakerPresent = doc["loudspeaker_present"].as<int>() != 0;
    }
    if (!doc["asrout"].isNull()) { int v=doc["asrout"].as<int>(); if(v==0||v==1)cfg.asroutEnabled=(uint8_t)v; }
    if (!doc["asrout_gain"].isNull()) { float v=doc["asrout_gain"].as<float>(); if(v>=0.0f&&v<=1000)cfg.asroutGain=v; }
    if (!doc["echo_suppression"].isNull()) { cfg.echoSuppressionEnabled = doc["echo_suppression"].as<bool>(); }
    // Fixed beams: Adaptive default — ignore inbound fixed-beam toggles.
    cfg.fixedBeamsEnabled = 0;
    cfg.fixedBeamGating = 0;
    if (!doc["attns_mode"].isNull()) { int v=doc["attns_mode"].as<int>(); if(v==0||v==1)cfg.attnsMode=(uint8_t)v; }
    if (!doc["attns_nominal"].isNull()) { float v=doc["attns_nominal"].as<float>(); if(v>=0&&v<=1)cfg.attnsNominal=v; }
    if (!doc["attns_slope"].isNull()) { float v=doc["attns_slope"].as<float>(); if(v>=0&&v<=10)cfg.attnsSlope=v; }
    if (!doc["ref_gain"].isNull()) { float v=doc["ref_gain"].as<float>(); if(v>=0.1f&&v<=1000)cfg.refGain=v; }
    if (!doc["sys_delay"].isNull()) { int v=doc["sys_delay"].as<int>(); if(v>=-10000&&v<=10000)cfg.sysDelaySamples=v; }
    syncHpfFields(cfg);
    sanitizeLoudspeakerOff(cfg);
    return true;
}

float NetConfig::hpfModeToHz(uint8_t mode) {
    static const float kHz[5] = {0.0f, 70.0f, 125.0f, 150.0f, 180.0f};
    if (mode > 4) mode = 2;
    return kHz[mode];
}

void NetConfig::syncHpfFields(NetConfigData &cfg) {
    if (cfg.hpfMode > 4) cfg.hpfMode = 2;
    cfg.hpfEnabled = (cfg.hpfMode != 0);
    cfg.hpfCutoffHz = hpfModeToHz(cfg.hpfMode);
}

void NetConfig::sanitizeLoudspeakerOff(NetConfigData &cfg) {
    if (cfg.loudspeakerPresent) return;
    cfg.echoSuppressionEnabled = false;
    cfg.asroutEnabled = 1;
}