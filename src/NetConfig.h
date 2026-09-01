/**
 * @file    NetConfig.h
 * @brief   Настройки устройства в NVS (namespace `rtspmic`).
 *
 * Загрузка/сохранение, schema_ver, soft-apply, factory/network reset.
 * Каталог ключей: docs/API_REFERENCE.md §5.
 */

#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Config.h"

#define NVS_SCHEMA_VERSION 1

/**
 * NVS / telemetry value для WebUI «Кольцо».
 * Публичный API — только on/off; см. docs/LED.md.
 * STATUS(0)=on → ESP Wi‑Fi pattern + XVF LED_EFFECT DoA(4).
 * OFF(1)=off → оба тёмные (XVF LED_EFFECT 0).
 * LEVEL(2)=legacy NVS; трактуется как on.
 */
enum LedMode : uint8_t {
    LED_MODE_STATUS = 0,  ///< публичный «on»
    LED_MODE_OFF    = 1,  ///< публичный «off»
    LED_MODE_LEVEL  = 2,  ///< deprecated → on
};

enum AecEnvMode : uint8_t {
    AEC_ENV_QUIET    = 0,  ///< тихо (за городом) — silence=1e-7
    AEC_ENV_NOISY    = 1,  ///< шумно (город) — silence=1e-5
};

struct SecurityControls {
    uint8_t ledMode;  ///< LedMode
};

/** HMAC-SHA256 ключ для подписи MQTT-команд (32 байта). */
struct HmacKey {
    uint8_t bytes[32];
    bool valid;
};

/** Полный снимок настроек (зеркало NVS + derived поля). */
struct NetConfigData {
    // ── LAN: MQTT ──
    bool     mqttEnabled;
    char     mqttHost[64];
    uint16_t mqttPort;
    char     mqttUser[32];
    char     mqttPass[64];
    bool     mqttHaDiscovery;      ///< Home Assistant MQTT Discovery

    // ── LAN: remote RTSP client ──
    bool     rtspRemoteEnabled;
    char     rtspRemoteHost[64];
    uint16_t rtspRemotePort;

    // ── NTP ──
    char     ntpHost[64];

    // ── Audio / HPF ──
    // XVF AEC_HPFONOFF: 0=Off, 1=70, 2=125, 3=150, 4=180 Гц (XMOS UG).
    uint8_t  hpfMode;          ///< 0..4
    bool     hpfEnabled;       ///< derived: hpfMode != 0 (legacy JSON/telemetry)
    float    hpfCutoffHz;      ///< derived Гц из hpfMode (0 если off)
    /** Режим настройки микрофона: локальный RTSP + DSP UI. */
    bool     audioSetupMode;

    // ── System ──
    char     timezone[48];
    bool     scheduledReset;
    uint8_t  scheduledResetHour;    ///< 0..23; default 3
    uint8_t  scheduledResetMinute;  ///< 0..59; default 0
    float    calibrationOffsetDb;   ///< смещение SPL, дБ

    // ── Security ──
    HmacKey  hmacKey;
    SecurityControls security;

    // ── DSP (XVF engineer) ──
    bool     dspAgcEnabled;
    float    dspNsStationary;      ///< PP_MIN_NS (1.0 = off)
    float    dspNsNonStationary;   ///< PP_MIN_NN
    float    dspAgcMaxGain;
    float    dspAgcDesiredLevel;
    bool     dspLimiterEnabled;
    float    dspLimiterThreshold;
    float    dspAgcTime;
    float    dspAgcFastTime;
    float    dspAgcAlphaSlow;
    float    dspAgcAlphaFast;
    float    dspMicGain;         ///< AUDIO_MGR_MIC_GAIN
    uint8_t  aecEnvMode;         ///< AecEnvMode
    bool     loudspeakerPresent; ///< иначе ASROUT/echo locked
    uint8_t  asroutEnabled;      ///< 0=AEC residuals, 1=beam/ASR на I2S
    float    asroutGain;         ///< AEC_ASROUTGAIN (linear)
    bool     echoSuppressionEnabled; ///< PP_ECHOONOFF
    uint8_t  fixedBeamsEnabled;  ///< AEC_FIXEDBEAMSONOFF
    float    fixedBeamAzDeg[2];  ///< углы B1/B2
    uint8_t  fixedBeamGating;    ///< AEC_FIXEDBEAMSGATING
    uint8_t  attnsMode;          ///< PP_ATTNS_MODE
    float    attnsNominal;       ///< PP_ATTNS_NOMINAL
    float    attnsSlope;         ///< PP_ATTNS_SLOPE
    float    refGain;            ///< AUDIO_MGR_REF_GAIN
    int32_t  sysDelaySamples;    ///< AUDIO_MGR_SYS_DELAY
};

struct SoftApplyResult {
    bool success;
    bool rebootRecommended;
    const char *errorMsg;
};

enum ResetScope : uint8_t {
    RESET_FACTORY = 0,
    RESET_NETWORK = 1,
};

class NetConfig {
public:
    static const char *NVS_NS;           ///< "rtspmic"
    static const uint8_t SCHEMA_VERSION;
    static const char *KEY_SCHEMA_VER;   ///< "schema_ver"

    static void load(NetConfigData &out);
    static bool save(const NetConfigData &in);
    static void applyDefaults(NetConfigData &out);
    static bool runMigration();
    static uint8_t getStoredVersion();

    /**
     * Применить изменения без полного reboot где возможно.
     * Флаги *Changed говорят, какие подсистемы перезапустить.
     */
    static SoftApplyResult softApply(const NetConfigData &cfg,
                                     bool mqttChanged,
                                     bool rtspChanged, bool ntpChanged,
                                     bool audioChanged, bool systemChanged,
                                     bool securityChanged);
    static bool factoryReset();
    static bool networkReset();
    static void toJson(JsonObject obj, const NetConfigData &cfg, bool maskSecrets = true);
    static bool fromJson(JsonVariantConst doc, NetConfigData &cfg);
    /** Без динамика: echo OFF + ASROUT=beam. */
    static void sanitizeLoudspeakerOff(NetConfigData &cfg);
    /** Clamp 0..4 и синхронизация hpfEnabled + hpfCutoffHz из hpfMode. */
    static void syncHpfFields(NetConfigData &cfg);
    static float hpfModeToHz(uint8_t mode);
};

#endif // NET_CONFIG_H
