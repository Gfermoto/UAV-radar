#include "CommandDispatcher.h"

#include <cstring>

#include "AudioProducer.h"
#include "NetConfig.h"
#include "SystemMonitor.h"
#include "TelemetryBuilder.h"
#include "XVF3800_I2C.h"

void CommandDispatcher::dispatch(const char *cmd, const JsonVariant &value) {
    if (!cmd || !_d.netCfg || !_d.xvf || !_d.audio || !_d.sysMonitor) return;

    NetConfigData &cfg = *_d.netCfg;

    if (strcmp(cmd, "mute") == 0 && value.is<bool>()) {
        _d.xvf->writeMute(value.as<bool>());
        Serial.printf("[CMD] Mute: %s\n", value.as<bool>() ? "ON" : "OFF");
    } else if (strcmp(cmd, "hpf") == 0 && value.is<bool>()) {
        _d.audio->setHpfEnabled(value.as<bool>());
        Serial.printf("[CMD] HPF (аппаратный 125 Гц): %s\n", value.as<bool>() ? "ON" : "OFF");
    } else if (strcmp(cmd, "aec_env") == 0 && value.is<int>()) {
        int env = value.as<int>();
        if (env == 0 || env == 1) {
            cfg.aecEnvMode = (uint8_t)env;
            float sl = (env == 0) ? 1e-7f : 1e-5f;
            _d.xvf->setSilenceLevel(sl);
            TelemetryBuilder::setAecEnvState((uint8_t)env);
            NetConfig::save(cfg);
            Serial.printf("[CMD] AEC env: %s (sl=%.0e)\n",
                          env == 0 ? "QUIET" : "NOISY", sl);
        }
    } else if (strcmp(cmd, "mic_gain") == 0 &&
               (value.is<float>() || value.is<double>() || value.is<int>())) {
        float g = value.as<float>();
        if (g >= 0.1f && g <= 1000.0f) {
            cfg.dspMicGain = g;
            _d.xvf->setMicGain(g);
            NetConfig::save(cfg);
            TelemetryBuilder::setDspMicGainState(g);
            if (_d.syncDspPathGains) _d.syncDspPathGains();
            Serial.printf("[CMD] Mic gain: %.1f\n", g);
        }
    } else if (strcmp(cmd, "led_mode") == 0) {
        // MQTT/WebUI: on|status|0|level|2 → STATUS(on); off|1 → OFF. docs/LED.md
        uint8_t mode = LED_MODE_STATUS;
        bool ok = false;
        if (value.is<const char *>()) {
            const char *m = value.as<const char *>();
            if (strcmp(m, "on") == 0 || strcmp(m, "status") == 0 ||
                strcmp(m, "0") == 0 || strcmp(m, "level") == 0 ||
                strcmp(m, "2") == 0) {
                mode = LED_MODE_STATUS;
                ok = true;
            } else if (strcmp(m, "off") == 0 || strcmp(m, "1") == 0) {
                mode = LED_MODE_OFF;
                ok = true;
            }
        } else if (value.is<int>()) {
            int lm = value.as<int>();
            if (lm == 1) {
                mode = LED_MODE_OFF;
                ok = true;
            } else if (lm == 0 || lm == 2) {
                mode = LED_MODE_STATUS;  // 2=level deprecated → on
                ok = true;
            }
        }
        if (ok) {
            cfg.security.ledMode = mode;
            NetConfig::save(cfg);
            if (_d.applyLedMode) _d.applyLedMode(mode);
            Serial.printf("[CMD] LED mode: %u\n", (unsigned)mode);
        }
    } else if (strcmp(cmd, "agc") == 0 && value.is<bool>()) {
        bool enable = value.as<bool>();
        cfg.dspAgcEnabled = enable;
        NetConfig::save(cfg);
        _d.xvf->applyEngineerConfig(cfg);
        TelemetryBuilder::setDspAgcState(enable);
        Serial.printf("[CMD] AGC: %s\n", enable ? "ON" : "OFF");
    } else if (strcmp(cmd, "apply_dsp") == 0) {
        NetConfig::load(cfg);
        _d.xvf->applyEngineerConfig(cfg);
        _d.audio->setHpfMode(cfg.hpfMode);
        TelemetryBuilder::setDspAgcState(cfg.dspAgcEnabled);
        TelemetryBuilder::setDspLimiterState(cfg.dspLimiterEnabled);
        TelemetryBuilder::setAecEnvState(cfg.aecEnvMode);
        TelemetryBuilder::setEchoSuppressionState(cfg.echoSuppressionEnabled);
        TelemetryBuilder::setAsroutState(cfg.asroutEnabled);
        TelemetryBuilder::setLoudspeakerPresentState(cfg.loudspeakerPresent);
        TelemetryBuilder::setDspMicGainState(cfg.dspMicGain);
        if (_d.syncDspPathGains) _d.syncDspPathGains();
        TelemetryBuilder::setCalibrationOffsetDb(cfg.calibrationOffsetDb);
        Serial.printf("[CMD] DSP engineer config applied\n");
    } else if (strcmp(cmd, "apply_system") == 0) {
        NetConfig::load(cfg);
        _d.sysMonitor->setScheduledResetEnabled(cfg.scheduledReset);
        _d.sysMonitor->setScheduledResetTime(cfg.scheduledResetHour, cfg.scheduledResetMinute);
        Serial.printf("[CMD] System applied (sched_reset=%s %02u:%02u)\n",
                      cfg.scheduledReset ? "ON" : "OFF",
                      (unsigned)cfg.scheduledResetHour,
                      (unsigned)cfg.scheduledResetMinute);
    }
}
