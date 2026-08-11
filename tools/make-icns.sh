#!/bin/bash
# Пересобрать res/icon.icns из res/icon.ico — иконку .app-бандла под macOS.
#
# Запуск из корня репозитория:  bash tools/make-icns.sh
#
# Готовый .icns лежит в репозитории, чтобы сборка на чистой машине не зависела
# от sips/iconutil. Гонять этот скрипт нужно только если поменялась icon.ico —
# иначе иконка бандла тихо разойдётся с иконкой Windows.
#
# ⚠️ Исходник 256×256, поэтому размера 512 и 1024 в наборе нет: Finder крупные
#    представления отрисует масштабированием. Хочешь резкости на больших
#    значках — нужен исходник крупнее, .ico его не содержит.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/res/icon.ico"
OUT="$REPO/res/icon.icns"

[ -f "$SRC" ] || { echo "ОШИБКА: нет $SRC" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SET="$WORK/icon.iconset"
mkdir -p "$SET"

# .ico -> png один раз, дальше только уменьшаем: апскейл дал бы мыло.
sips -s format png "$SRC" --out "$WORK/base.png" >/dev/null
SIZE="$(sips -g pixelWidth "$WORK/base.png" | awk '/pixelWidth/{print $2}')"
echo "исходник: ${SIZE}×${SIZE}"

# iconutil требует ровно этих имён; @2x — та же картинка вдвое крупнее.
gen() { sips -z "$2" "$2" "$WORK/base.png" --out "$SET/icon_$1.png" >/dev/null; }
gen 16x16       16
gen 16x16@2x    32
gen 32x32       32
gen 32x32@2x    64
gen 128x128    128
gen 128x128@2x 256
gen 256x256    256

iconutil -c icns "$SET" -o "$OUT"
echo "готово: $OUT ($(du -h "$OUT" | cut -f1))"
