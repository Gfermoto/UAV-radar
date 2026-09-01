# Кольцо светодиодов · LED ring

**[Русский](#русский)** · **[English](#english)**

На плате два разных индикатора, но в WebUI оба подписаны **Кольцо / Ring**. Не путайте.

---

<a id="русский"></a>
## Русский

| Индикатор | Железо | Драйвер | Вкл | Выкл |
|-----------|--------|---------|-----|------|
| **RGB-кольцо mic** | 12× WS2812 на Seeed XVF3800 | I2C GPO ResID **20**, `LED_EFFECT` (12) | effect **4** (DoA) | effect **0** |
| **LED статуса ESP** | XIAO **GPIO21** | `LedIndicator` | Wi‑Fi OK / fail | выкл |

### Управление

- WebUI → Система → **Кольцо** → Вкл / Выкл  
- HTTP: `POST /api/system/led` `mode=on|off` (Basic + CSRF)  
  Алиасы: `status`/`0` → вкл; старый `level`/`2` → вкл  
- MQTT: `led_mode` = `on` / `off`  
- NVS `sec_led_mode`: `0` = вкл, `1` = выкл (`2` legacy → вкл)

### Режимы `LED_EFFECT` (Seeed)

Из [reSpeaker host_control](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/host_control/README.md):

| Value | Режим |
|------:|-------|
| 0 | выкл |
| 1 | breath |
| 2 | rainbow |
| 3 | solid color |
| 4 | **doa** |

«Вкл» в WebUI возвращает **DoA** (как у Seeed после заводской прошивки), не радугу и не «моргает всё кольцо». Красный LED mute — GPO пин **30**, это не RGB-кольцо.

### Путь в коде

```text
WebUI / MQTT → led_mode → applyLedMode()
                 ├─ LedIndicator (GPIO21)
                 └─ setLedRingEnabled() → LED_EFFECT 0 or 4
```

### Проверка на плате

1. Кольцо Вкл → DoA следует речи (не залипшее свечение всего кольца).  
2. Кольцо Выкл → темно; UART: `XVF ring off (effect 0) → OK`.  
3. GPIO21 показывает Wi‑Fi, когда режим кольца «вкл».

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

«On» restores **doa** (Seeed factory default), not a full-ring blink. Mute red LED is GPO pin **30** — not this ring.

### Code path

```text
WebUI / MQTT → led_mode → applyLedMode()
                 ├─ LedIndicator (GPIO21)
                 └─ setLedRingEnabled() → LED_EFFECT 0 or 4
```

### Quick check

1. Ring On → DoA moves with speech (not all LEDs solid).  
2. Ring Off → dark; UART: `XVF ring off (effect 0) → OK`.  
3. GPIO21 still shows Wi‑Fi when ring mode is on.
