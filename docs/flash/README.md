# Web flash (лендинг)

## ESP (XIAO)

ESP Web Tools шьёт **этот** `firmware-nevod_diy.bin` (factory: bootloader + partitions + app на `0x0`).

Flash mode **DIO** — как у `pio run -t upload`. Не QIO: merge с QIO ломает старт XIAO.

Путь в `manifest.json` только относительный. Файл с GitHub Releases (`diy-ota`) — OTA-приложение, без CORS и **не** для `0x0`.

Пересобрать (после promote `diy-ota` это делает Actions `sync-web-flash.yml`):

```bash
python3 tools/sync_web_flash.py --app /path/to/firmware-nevod_diy.bin --version 0.17.2
```

## DSP (XVF3800)

Страница [`../dsp.html`](../dsp.html) — WebDFU (Chrome/Edge).

| Файл | Роль |
|------|------|
| `application_xvf3800_i2s_slave_v1.0.8_16k.bin` | Seeed I²S slave 16 kHz Upgrade |
| SHA-256 | `9dc3308a4db8570603bcc88103d2f0de0291cc92a384d2b25de6eef6f2d99eb8` |

Только **alt=1**. Не master 48 kHz. JS: `assets/dfu.js` (webdfu) + `assets/dfu-nevod.js`.
