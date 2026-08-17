# API & config reference · Справочник API и настроек

**[English](#english)** · **[Русский](#русский)**

Companion to [`openapi-webui.yaml`](../openapi-webui.yaml). Architecture: [ARCHITECTURE.md](ARCHITECTURE.md). LEDs: [LED.md](LED.md).

---

<a id="english"></a>
## English

### Ports

| Service | URL / port |
|---------|------------|
| WebUI | `http://<ip>/` (:80) |
| WebSocket telemetry | `/ws` (after ticket) |
| Local RTSP | `rtsp://<ip>:554/` |
| Captive AP | `http://192.168.4.1/` |
| MQTT broker | usually `:1883` (configurable) |

WebUI and RTSP share Basic credentials (`web_user` / `web_pass` in NVS). Default password `rtsp-mic-change-me` — **change it**.

A nature stream from this mic can feed [BirdNET](https://github.com/kahst/BirdNET-Analyzer) or similar tools. UAV detection is the separate NEVOD path.

### WebUI auth (short)

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

Templates in [`Config.h`](../src/Config.h); `{node_id}` is filled at runtime.

| Topic | Dir | Purpose |
|-------|-----|---------|
| `…/telemetry` | publish | JSON ~1 Hz |
| `…/status` | publish | node status |
| `…/events` | publish | events |
| `…/command` | subscribe | commands |

Command signing: NVS `hmac_key` (64 hex). Empty key → **all commands rejected**. See `CommandAuth.h` (legacy + nonce v2). Before NTP sync → fail-closed.

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

**Network:** `mqtt_*`, `rtsp_*` (remote client), `ntp_host`, `aud_setup`  
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

---

<a id="русский"></a>
## Русский

Кратко то же самое, что выше.

**Порты:** WebUI `:80`, RTSP `:554`, WS `/ws`, captive `192.168.4.1`, MQTT часто `:1883`.  
Общий Basic для Web и RTSP; пароль по умолчанию смените сразу.

Поток mic можно отдать в [BirdNET](https://github.com/kahst/BirdNET-Analyzer) и похожие инструменты. Детекция БПЛА — отдельный путь NEVOD.

**Auth WebUI:** public SPA → Basic → mutable+CSRF → hardened (не-default пароль) → WS ticket.

**MQTT:** топики `rtsp-mic/v1/{node_id}/…`; без валидного `hmac_key` команды не принимаются; до NTP — отказ.

**Команды:** `mute`, `hpf`, `aec_env`, `mic_gain`, `led_mode`, `agc`, `apply_dsp`, `apply_system` — см. таблицу в английской секции.

**NVS `rtspmic`:** ключи сети, DSP, системы и `web_user`/`web_pass` — см. списки выше. Factory reset чистит этот namespace.
