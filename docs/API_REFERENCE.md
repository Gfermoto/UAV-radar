# Справочник API и настроек · API & config reference

**[Русский](#русский)** · **[English](#english)**

Дополнение к [`openapi-webui.yaml`](../openapi-webui.yaml). Архитектура: [ARCHITECTURE.md](ARCHITECTURE.md). Светодиоды: [LED.md](LED.md).

---

<a id="русский"></a>
## Русский

Справочник для **открытой прошивки mic** (RTSP / WebUI / MQTT). Детекция БПЛА — в бинарнике NEVOD DIY, не в этом API.

### Порты

| Сервис | URL / порт |
|--------|------------|
| WebUI | `http://<ip>/` (:80) |
| WebSocket телеметрия | `/ws` (после ticket) |
| Локальный RTSP | `rtsp://<ip>:554/` |
| Captive AP | `http://192.168.4.1/` |
| MQTT брокер | обычно `:1883` (настраивается) |

WebUI и RTSP — один Basic (`web_user` / `web_pass` в NVS). Пароль по умолчанию `rtsp-mic-change-me` — **смените**.

Поток mic можно отдать в [BirdNET](https://github.com/kahst/BirdNET-Analyzer). Детекция БПЛА — отдельная прошивка NEVOD.

### Авторизация WebUI

Подробности в OpenAPI `info.description`.

| Уровень | Смысл |
|---------|--------|
| Public | только SPA `GET /` |
| Basic | `/status` и большинство GET |
| Mutable | POST + CSRF (`X-CSRF-Token`) |
| Hardened | Basic + не-default пароль |
| WS | ticket из `/api/ws-ticket`, затем `{"auth":"…"}` на `/ws` |

С default-паролем мутации блокируются (`must_change_password`).

### MQTT топики

Шаблоны в [`Config.h`](../src/Config.h); `{node_id}` подставляется в runtime.

| Топик | Направление | Назначение |
|-------|-------------|------------|
| `…/telemetry` | publish | JSON ~1 Гц |
| `…/status` | publish | статус узла |
| `…/events` | publish | события |
| `…/command` | subscribe | команды |

Подпись команд: NVS `hmac_key` (64 hex). Пустой ключ → **все команды отклоняются**. См. `CommandAuth.h`. До синхронизации NTP → отказ.

### Команды (`CommandDispatcher`)

| cmd | value | Действие |
|-----|-------|----------|
| `mute` | bool | mute GPO на XVF |
| `hpf` | bool | HPF вкл/выкл |
| `aec_env` | 0\|1 | тихий / шумный уровень тишины |
| `mic_gain` | 0.1…1000 | gain + NVS + телеметрия |
| `led_mode` | on/off | см. [LED.md](LED.md) |
| `agc` | bool | AGC |
| `apply_dsp` | any | перечитать DSP из NVS |
| `apply_system` | any | отложенный reset → SystemMonitor |

### NVS namespace `rtspmic`

Ключ схемы `schema_ver` = 1. Реализация: `NetConfig`.

**Сеть:** `mqtt_*`, `rtsp_*`, `ntp_host`, `aud_setup`  
**DSP / звук:** `hpf_*`, `dsp_*`, `aec_env`, `loudspeaker_en`, `asrout_*`, `echo_en`, `fixed_*`, `attns_*`, `ref_gain`, `sys_delay`  
**Система:** `timezone`, `sched_reset*`, `cal_offset`, `hmac_key`, `sec_led_mode`  
**Web:** `web_user`, `web_pass`

Factory reset очищает `rtspmic`. Wi‑Fi STA — отдельно (`WiFiSetup`).

### Карта кода

| Тема | Файлы |
|------|--------|
| Команды | `CommandDispatcher.cpp` |
| MQTT | `MQTTManager.*`, `CommandAuth.h` |
| NVS | `NetConfig.*` |
| Учётные данные | `WebCredentials.*` |
| Константы | `Config.h` |

---

<a id="english"></a>
## English

Reference for the **open mic firmware** (RTSP / WebUI / MQTT). UAV detection lives in the NEVOD DIY binary, not this API surface.

### Ports

| Service | URL / port |
|---------|------------|
| WebUI | `http://<ip>/` (:80) |
| WebSocket telemetry | `/ws` (after ticket) |
| Local RTSP | `rtsp://<ip>:554/` |
| Captive AP | `http://192.168.4.1/` |
| MQTT broker | usually `:1883` (configurable) |

WebUI and RTSP share Basic credentials (`web_user` / `web_pass` in NVS). Default password `rtsp-mic-change-me` — **change it**.

A nature stream can feed [BirdNET](https://github.com/kahst/BirdNET-Analyzer). UAV detection is the separate NEVOD firmware path.

### WebUI auth

Full detail in OpenAPI `info.description`.

| Level | Meaning |
|-------|---------|
| Public | SPA `GET /` only |
| Basic | `/status` and most GETs |
| Mutable | POST + CSRF (`X-CSRF-Token`) |
| Hardened | Basic + non-default password |
| WS | ticket from `/api/ws-ticket`, then `{"auth":"…"}` on `/ws` |

Default password blocks mutations (`must_change_password`).

### MQTT topics

Templates in [`Config.h`](../src/Config.h); `{node_id}` at runtime.

| Topic | Dir | Purpose |
|-------|-----|---------|
| `…/telemetry` | publish | JSON ~1 Hz |
| `…/status` | publish | node status |
| `…/events` | publish | events |
| `…/command` | subscribe | commands |

Command signing: NVS `hmac_key` (64 hex). Empty key → **all commands rejected**. See `CommandAuth.h`. Before NTP sync → fail-closed.

### Commands (`CommandDispatcher`)

| cmd | value | Effect |
|-----|-------|--------|
| `mute` | bool | XVF mute GPO |
| `hpf` | bool | HPF on/off |
| `aec_env` | 0\|1 | quiet / noisy silence level |
| `mic_gain` | 0.1…1000 | mic gain + NVS + telemetry sync |
| `led_mode` | on/off | see [LED.md](LED.md) |
| `agc` | bool | AGC |
| `apply_dsp` | any | reload DSP from NVS |
| `apply_system` | any | scheduled reset → SystemMonitor |

### NVS namespace `rtspmic`

Schema key `schema_ver` = 1. Implementation: `NetConfig`.

**Network:** `mqtt_*`, `rtsp_*`, `ntp_host`, `aud_setup`  
**DSP / audio:** `hpf_*`, `dsp_*`, `aec_env`, `loudspeaker_en`, `asrout_*`, `echo_en`, `fixed_*`, `attns_*`, `ref_gain`, `sys_delay`  
**System:** `timezone`, `sched_reset*`, `cal_offset`, `hmac_key`, `sec_led_mode`  
**Web:** `web_user`, `web_pass`

Factory reset clears `rtspmic`. Wi‑Fi STA creds are separate (`WiFiSetup`).

### Source map

| Topic | Files |
|-------|--------|
| Commands | `CommandDispatcher.cpp` |
| MQTT | `MQTTManager.*`, `CommandAuth.h` |
| NVS | `NetConfig.*` |
| Creds | `WebCredentials.*` |
| Constants | `Config.h` |
