# Web flash (лендинг)

ESP Web Tools шьёт **этот** `firmware-nevod_diy.bin` (factory: bootloader + partitions + app на `0x0`).

Flash mode **DIO** — как у `pio run -t upload`. Не QIO: merge с QIO ломает старт XIAO.

Путь в `manifest.json` только относительный. Файл с GitHub Releases (`diy-ota`) — OTA-приложение, без CORS и **не** для `0x0`.

Пересобрать (после promote `diy-ota` это делает Actions `sync-web-flash.yml`):

```bash
python3 tools/sync_web_flash.py --app /path/to/firmware-nevod_diy.bin --version 0.17.2
```
