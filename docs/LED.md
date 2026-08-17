# LED ring · Кольцо светодиодов

**[English](#english)** · **[Русский](#русский)**

Two indicators share the WebUI label **Ring / Кольцо**. Do not mix them up.

---

<a id="english"></a>
## English

| Indicator | Hardware | Driver | On | Off |
|-----------|----------|--------|----|-----|
| **Mic RGB ring** | 12× WS2812 on Seeed XVF3800 | I2C GPO ResID **20**, `LED_EFFECT` (12) | effect **4** (DoA) | effect **0** |
| **ESP status LED** | XIAO **GPIO21** | `LedIndicator` | Wi‑Fi OK / fail blink | off |

### How to control

- WebUI → System → **Ring** → On / Off  
- HTTP: `POST /api/system/led` `mode=on|off` (Basic + CSRF)  
  Aliases: `status`/`0` → on; old `level`/`2` → on  
- MQTT: `led_mode` = `on` / `off`  
- NVS `sec_led_mode`: `0` = on, `1` = off (`2` legacy → on)

### Seeed `LED_EFFECT`

From [reSpeaker host_control](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/host_control/README.md):

| Value | Mode |
|------:|------|
| 0 | off |
| 1 | breath |
| 2 | rainbow |
| 3 | solid color |
| 4 | **doa** |

«On» restores **doa** (factory default), not a full-ring blink. Mute’s red LED is GPO pin **30** — not this ring.

### Code path

```text
WebUI / MQTT → led_mode → applyLedMode()
                 ├─ LedIndicator (GPIO21)
                 └─ setLedRingEnabled() → LED_EFFECT 0 or 4
```

### Quick check

1. Ring On → DoA moves with speech (not a solid flash of all LEDs).  
2. Ring Off → dark; UART: `XVF ring off (effect 0) → OK`.  
3. GPIO21 still shows Wi‑Fi when ring mode is on.

---

<a id="русский"></a>
## Русский

| Индикатор | Железо | Драйвер | Вкл | Выкл |
|-----------|--------|---------|-----|------|
| **RGB-кольцо mic** | 12× WS2812 на Seeed | I2C GPO **20**, `LED_EFFECT` (12) | effect **4** (DoA) | effect **0** |
| **LED статуса ESP** | XIAO **GPIO21** | `LedIndicator` | Wi‑Fi OK / fail | выкл |

### Управление

- WebUI → Система → **Кольцо**  
- HTTP: `POST /api/system/led` `mode=on|off`  
- MQTT: `led_mode`  
- NVS `sec_led_mode`: `0` вкл, `1` выкл

«Вкл» = режим **DoA** (как после заводской прошивки Seeed), не радуга и не «моргает всё кольцо». Красный LED mute — пин **30**, это другое.

Проверка по UART и карта файлов — как в английской секции; см. также [ARCHITECTURE.md](ARCHITECTURE.md).
