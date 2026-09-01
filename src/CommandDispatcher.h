/**
 * @file    CommandDispatcher.h
 * @brief   Единый обработчик команд MQTT + WebUI (Core 0).
 *
 * Имена `cmd` и типы `value` общие для обоих каналов.
 * Каталог (см. также docs/API_REFERENCE.md §4):
 *
 * | cmd           | value              | действие                          |
 * |---------------|--------------------|-----------------------------------|
 * | mute          | bool               | XVF GPO mute                      |
 * | hpf           | bool               | HPF on/off                        |
 * | aec_env       | int 0\|1           | quiet/noisy silence level         |
 * | mic_gain      | number 0.1…1000    | AUDIO_MGR_MIC_GAIN + NVS          |
 * | led_mode      | string/int on\|off | Кольцо + GPIO21 (docs/LED.md)     |
 * | agc           | bool               | AGC + applyEngineerConfig         |
 * | apply_dsp     | any                | reload NVS DSP → XVF/audio/telem  |
 * | apply_system  | any                | scheduled reset → SystemMonitor   |
 */

#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <ArduinoJson.h>

struct NetConfigData;
class XVF3800_I2C;
class AudioProducer;
class SystemMonitor;

class CommandDispatcher {
public:
    /** Применить LedMode к GPIO21 + XVF ring (main::applyLedMode). */
    using ApplyLedModeFn = void (*)(uint8_t mode);
    using SyncDspPathGainsFn = void (*)();

    struct Deps {
        NetConfigData *netCfg = nullptr;
        XVF3800_I2C *xvf = nullptr;
        AudioProducer *audio = nullptr;
        SystemMonitor *sysMonitor = nullptr;
        ApplyLedModeFn applyLedMode = nullptr;
        SyncDspPathGainsFn syncDspPathGains = nullptr;
    };

    explicit CommandDispatcher(const Deps &deps) : _d(deps) {}

    /**
     * Выполнить команду. Неизвестные cmd и неверный тип value — no-op.
     * @param cmd  NUL-terminated имя команды
     * @param value JSON variant (bool/number/string по каталогу)
     */
    void dispatch(const char *cmd, const JsonVariant &value);

private:
    Deps _d;
};

#endif  // COMMAND_DISPATCHER_H
