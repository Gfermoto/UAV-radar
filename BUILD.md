# Build & flash · Сборка и прошивка

**[English](#english)** · **[Русский](#русский)**

---

<a id="english"></a>
## English

### Requirements

- [PlatformIO](https://platformio.org/) Core or the VS Code / Cursor extension
- USB access to a Seeed XIAO ESP32-S3
- Python 3 only if you regenerate MEL LUTs under `tools/`

### Environment

Firmware env: **`rtsp_mic`**

| Setting | Value |
|---------|--------|
| Board | `seeed_xiao_esp32s3` |
| Flash / PSRAM | 8 MB / OPI |
| Ethernet | compiled in (cable optional) |
| RTSP | `:554` |
| WebUI | `:80` |
| MQTT | `:1883` (optional) |

### Pins (XIAO + XVF3800)

From [`src/Config.h`](src/Config.h):

| Function | GPIO |
|----------|------|
| I2S BCLK | 8 |
| I2S WS | 7 |
| I2S DATA_IN | 43 |
| I2C SDA (XVF) | 5 (D4) |
| I2C SCL (XVF) | 6 (D5) |
| Status LED (XIAO) | 21 |
| Factory-reset BOOT | 0 (hold ≥ 3 s; see WiFiSetup CDC disarm) |
| ETH CS / INT / RST | 10 / 11 / 12 |
| SPI SCK / MOSI / MISO | 13 / 15 / 14 |

XVF I2C address `0x2C`. The mic RGB ring is **not** GPIO21 — see [docs/LED.md](docs/LED.md).

### Commands

```bash
pio run -e rtsp_mic
pio run -e rtsp_mic -t upload --upload-port /dev/ttyACM0
pio device monitor -e rtsp_mic -b 115200
```

English WebUI strings: add `-DWEBUI_LANG_EN` to `build_flags`.

MEL LUT regen (after filterbank changes):

```bash
python3 tools/generate_mel_lut.py
```

### First boot

1. Join Wi‑Fi (captive portal or saved creds) or plug Ethernet.
2. Open `http://<ip>/`, log in with `rtsp-mic-change-me`, then **change the password**.
3. In VLC: Media → Open Network → `rtsp://<ip>:554/`

Settings live in NVS namespace `rtspmic`. After major upgrades, a factory reset helps migrations.

NVS / MQTT: [docs/API_REFERENCE.md](docs/API_REFERENCE.md). Architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Loudspeaker / AEC

The «loudspeaker / monitor» checkbox enables echo suppression and the ASROUT path. Leave it off if you are not driving a speaker.

### LED ring (on device)

WebUI **Ring** controls the Seeed RGB ring (`LED_EFFECT` 0 = off, 4 = DoA), not only GPIO21. UART should show:

```text
[LED] XVF ring doa (effect 4) → OK
[LED] XVF ring off (effect 0) → OK
```

### Tests

```bash
pio test -e native -f test_native
```

On hardware: [tools/hil/README.md](tools/hil/README.md).

```bash
python3 tools/hil/test_device_hil.py --host <ip>
python3 tools/hil/test_rtsp_options.py --host <ip> --spa
```

---

<a id="русский"></a>
## Русский

### Что нужно

- [PlatformIO](https://platformio.org/) или расширение VS Code / Cursor
- USB к Seeed XIAO ESP32-S3
- Python 3 — только если пересобираете MEL LUT в `tools/`

### Окружение

Прошивка: **`rtsp_mic`**

| Параметр | Значение |
|----------|----------|
| Плата | `seeed_xiao_esp32s3` |
| Flash / PSRAM | 8 MB / OPI |
| Ethernet | в прошивке (кабель по желанию) |
| RTSP | `:554` |
| WebUI | `:80` |
| MQTT | `:1883` (по желанию) |

### Пины (XIAO + XVF3800)

Из [`src/Config.h`](src/Config.h):

| Функция | GPIO |
|---------|------|
| I2S BCLK | 8 |
| I2S WS | 7 |
| I2S DATA_IN | 43 |
| I2C SDA (XVF) | 5 (D4) |
| I2C SCL (XVF) | 6 (D5) |
| Status LED (XIAO) | 21 |
| Factory-reset BOOT | 0 (удержать ≥ 3 с; см. WiFiSetup / CDC) |
| ETH CS / INT / RST | 10 / 11 / 12 |
| SPI SCK / MOSI / MISO | 13 / 15 / 14 |

Адрес XVF `0x2C`. RGB-кольцо микрофона — **не** GPIO21; см. [docs/LED.md](docs/LED.md).

### Команды

```bash
pio run -e rtsp_mic
pio run -e rtsp_mic -t upload --upload-port /dev/ttyACM0
pio device monitor -e rtsp_mic -b 115200
```

Английский интерфейс WebUI: `-DWEBUI_LANG_EN` в `build_flags`.

Пересборка MEL LUT:

```bash
python3 tools/generate_mel_lut.py
```

### Первый запуск

1. Wi‑Fi (captive или сохранённые данные) либо Ethernet.
2. `http://<ip>/`, пароль `rtsp-mic-change-me`, затем **смените пароль**.
3. VLC → Сеть → `rtsp://<ip>:554/`

Настройки в NVS `rtspmic`. После крупных апгрейдов полезен factory reset.

NVS / MQTT: [docs/API_REFERENCE.md](docs/API_REFERENCE.md). Архитектура: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Динамик / AEC

Галочка «громкоговоритель / монитор» включает подавление эха и путь ASROUT. Без динамика оставьте выключенной.

### Кольцо (на плате)

В WebUI **Кольцо** — это RGB Seeed (`LED_EFFECT` 0 / 4), не только GPIO21. В UART:

```text
[LED] XVF ring doa (effect 4) → OK
[LED] XVF ring off (effect 0) → OK
```

### Тесты

```bash
pio test -e native -f test_native
```

На железе: [tools/hil/README.md](tools/hil/README.md).
