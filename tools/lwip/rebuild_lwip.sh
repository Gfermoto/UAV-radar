#!/usr/bin/env bash
# rebuild_lwip.sh — пересборка liblwip.a с увеличенным пулом TCP PCB
set -euo pipefail

SDK_LIB="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/lib"
ARCHIVE="${SDK_LIB}/liblwip.a"
BACKUP="${SDK_LIB}/liblwip.a.bak"
IDF="${HOME}/.platformio/packages/framework-espidf"
TC="${HOME}/.platformio/packages/toolchain-xtensa-esp32s3"
SDK="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3"
CC="${TC}/bin/xtensa-esp32s3-elf-gcc"
AR="${TC}/bin/xtensa-esp32s3-elf-ar"
BUILD="/tmp/lwip-rebuild"
OBJ="${BUILD}/obj"
INCS_FILE="${BUILD}/lwip_incs.txt"

# Получить include-пути если нет
if [ ! -f "$INCS_FILE" ]; then
    echo "Сборка include-путей..."
    cd /home/gfer/rtsp_mic_vfx3800
    rm -f .pio/build/rtsp_mic/src/WebUI.cpp.o
    pio run -e rtsp_mic -v 2>&1 | grep -m1 'xtensa.*WebUI.cpp' > /tmp/pio_cmd.txt 2>&1
    python3 -c "
import re
with open('/tmp/pio_cmd.txt') as f:
    text = f.read()
incs = re.findall(r'-I(\S+)', text)
for i in sorted(set(incs)):
    print(i)
" > "$INCS_FILE"
fi

# Патч sdkconfig.h
python3 -c "
import shutil
h = '$SDK/qio_opi/include/sdkconfig.h'
bk = h + '.lwipbak'
if not __import__('os').path.exists(bk):
    shutil.copy(h, bk)
with open(h) as f:
    lines = f.readlines()
changes = {
    'CONFIG_LWIP_TCP_MSL': 15000,
    'CONFIG_TCP_MSL': 15000,
    'CONFIG_LWIP_TCP_MEMP_NUM_TCP_PCB': 32,
    'CONFIG_TCP_MEMP_NUM_TCP_PCB': 32,
}
out = []
seen = set()
for line in lines:
    matched = False
    for key, val in changes.items():
        if line.startswith(f'#define {key} '):
            out.append(f'#define {key} {val}\n')
            seen.add(key)
            matched = True
            break
    if not matched:
        out.append(line)
last_lwip = max(i for i, l in enumerate(out) if 'CONFIG_LWIP_' in l)
for key, val in changes.items():
    if key not in seen:
        out.insert(last_lwip + 1, f'#define {key} {val}\n')
with open(h, 'w') as f:
    f.writelines(out)
print(f'Patched {len(seen)} keys')
"

# Восстановить бэкап
cp "$BACKUP" "$ARCHIVE"
echo "Бэкап восстановлен"

# Компилировать
mkdir -p "$OBJ"
INCS=$(cat "$INCS_FILE" | sed 's/^/-I/' | tr '\n' ' ')
CFLAGS="-mlongcalls -Os -w -DHAVE_CONFIG_H -DESP_PLATFORM -DESP32 -DESP32S3"
LWIP_SRC="$IDF/components/lwip"
OK=0
FAIL=0

for f in $LWIP_SRC/lwip/src/core/*.c $LWIP_SRC/lwip/src/core/ipv4/*.c \
         $LWIP_SRC/lwip/src/core/ipv6/*.c $LWIP_SRC/lwip/src/api/*.c \
         $LWIP_SRC/lwip/src/netif/*.c $LWIP_SRC/port/esp32/*.c \
         $LWIP_SRC/port/esp32/netif/*.c $LWIP_SRC/port/esp32/freertos/*.c \
         $LWIP_SRC/port/esp32/hooks/*.c; do
    bn=$(basename "$f" .c)
    if $CC $CFLAGS $INCS -c "$f" -o "$OBJ/$bn.o" 2>/dev/null; then
        OK=$((OK+1))
    else
        FAIL=$((FAIL+1))
    fi
done
echo "Скомпилировано: $OK, пропущено: $FAIL"

# Заменить в архиве (ar d + ar r)
for f in "$OBJ"/*.o; do
    bn=$(basename "$f" .o)
    # ar d удаляет первое вхождение — для нас это и есть старый объект
    $AR d "$ARCHIVE" "${bn}.c.obj" 2>/dev/null || true
    # переименовываем .o → .c.obj и добавляем
    cp "$f" "${OBJ}/${bn}.c.obj"
    $AR r "$ARCHIVE" "${OBJ}/${bn}.c.obj" 2>/dev/null || true
done

echo "Готово: $($AR t "$ARCHIVE" | wc -l) членов в архиве"