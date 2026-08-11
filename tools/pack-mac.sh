#!/bin/bash
# Упаковка релизного архива SignalBox под macOS.
#
# Запуск из корня репозитория:   bash tools/pack-mac.sh
# На выходе:                     dist/SignalBox-<версия>-mac.zip
#
# Что важно и почему:
#   * состав архива задан СПИСКОМ РАЗРЕШЁННОГО, а не списком исключений. Первый
#     же релиз проекта уехал с cameras.txt внутри и затёр бы список камер студии
#     (§7 документации); при обратной логике достаточно забыть один файл;
#   * внутри архива обязательна папка SignalBox/ — установщик обновления ищет
#     <распаковано>/SignalBox/SignalBox;
#   * метка «-mac» в имени файла обязательна: по ней приложение выбирает свою
#     сборку из релиза, где лежат обе (см. src/UpdateAsset.h);
#   * карантин снимается: dylib Sony приезжают из загруженного браузером пакета
#     SDK вместе с com.apple.quarantine, и этот атрибут переживает копирование.
#
# ⚠️ Архив НЕ подписан и НЕ нотаризован. На чужом Mac Gatekeeper будет ругаться
#    на каждую библиотеку Sony (они и от Sony идут только с ad-hoc подписью).
#    Для продаж нужен Apple Developer ID: подписать бинарник и КАЖДУЮ dylib,
#    затем notarytool + stapler. Скрипт до этого места намеренно не доходит.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build-mac"
DIST="$REPO/dist"

cd "$REPO"

# --- версия берётся из исходника, чтобы имя архива и тег релиза не разошлись ---
VERSION="$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' src/main.cpp | head -1)"
if [ -z "$VERSION" ]; then
    echo "ОШИБКА: не нашёл kAppVersion в src/main.cpp" >&2
    exit 1
fi

STAGE="$DIST/stage-mac"
PAYLOAD="$STAGE/SignalBox"          # именно эта папка окажется внутри архива
ARCHIVE="$DIST/SignalBox-$VERSION-mac.zip"

echo "SignalBox $VERSION — сборка архива под macOS"

if [ ! -x "$BUILD/SignalBox" ]; then
    echo "ОШИБКА: нет $BUILD/SignalBox — сначала собери:" >&2
    echo "  cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release && cmake --build build-mac" >&2
    exit 1
fi

# Архив собираем из свежего каталога, иначе в него незаметно переезжает мусор
# прошлой упаковки.
rm -rf "$STAGE"
mkdir -p "$PAYLOAD"

echo "  кладу программу и библиотеки"
cp "$BUILD/SignalBox" "$PAYLOAD/"
cp "$BUILD"/*.dylib "$PAYLOAD/"

echo "  кладу адаптеры (Contents/Frameworks/CrAdapter — на macOS ищутся только там)"
mkdir -p "$PAYLOAD/Contents/Frameworks"
cp -R "$BUILD/Contents/Frameworks/CrAdapter" "$PAYLOAD/Contents/Frameworks/"

# Панель — из build/Release/www: это источник правды под git, а build-mac/www
# лишь его копия, которую CMake перезаписывает на каждой сборке.
echo "  кладу панель www/ (из build/Release/www)"
cp -R "$REPO/build/Release/www" "$PAYLOAD/www"

for extra in "ЧИТАЙ-МЕНЯ.txt" "README.txt"; do
    if [ -f "$REPO/build/Release/$extra" ]; then
        echo "  кладу $extra"
        cp "$REPO/build/Release/$extra" "$PAYLOAD/"
    fi
done

# --- страховка: данные установки в архив попасть не должны ---
# Их создаёт сама программа, а robocopy/rsync при обновлении не удаляют лишнее в
# приёмнике — значит попавший в архив cameras.txt затёр бы список камер студии.
echo "  проверяю, что данных установки внутри нет"
LEAKED=""
for bad in cameras.txt cameras-known.txt groups.json settings.json firewall-skip.txt; do
    if [ -e "$PAYLOAD/$bad" ]; then LEAKED="$LEAKED $bad"; fi
done
if find "$PAYLOAD" -name '*.log' -o -name '*.log.prev' | grep -q .; then
    LEAKED="$LEAKED логи"
fi
if [ -n "$LEAKED" ]; then
    echo "ОШИБКА: в архив попали данные установки:$LEAKED" >&2
    exit 1
fi

# У xattr на macOS 15 нет рекурсивного режима (-r не понимает), поэтому обходим
# файлы через find. Чистим ДО упаковки: иначе атрибуты уедут в архив.
echo "  снимаю карантин (com.apple.quarantine приезжает вместе с dylib Sony)"
find "$PAYLOAD" -exec xattr -c {} \; 2>/dev/null || true

# ditto, а не zip: родной инструмент macOS, сохраняет права и симлинки, и ровно
# им же приложение архив распаковывает (plat::extractArchive).
# Без --sequesterRsrc: с ним ditto кладёт в архив отдельную папку __MACOSX.
#
# ℹ️ Файлы «._имя» внутри архива всё равно будут, и это НОРМАЛЬНО: xattr -c не
# снимает com.apple.provenance (системный атрибут, пользователю не отдаётся), а
# ditto хранит его рядом с файлом. При распаковке ditto и Архиватором macOS они
# склеиваются обратно — проверено, в распакованном дереве их ноль. Карантин при
# этом снят, а он единственный, кто мешает запуску.
echo "  пакую $ARCHIVE"
rm -f "$ARCHIVE"
( cd "$STAGE" && ditto -c -k --keepParent SignalBox "$ARCHIVE" )
rm -rf "$STAGE"

# --- самопроверка: распаковать так же, как это сделает установщик, и убедиться,
# что раскладка та, которую он ждёт. Дешевле минуты здесь, чем сломанное
# обновление у студий: §7 на выпуске релизов спотыкался уже дважды.
echo "  проверяю архив распаковкой"
CHECK="$DIST/check-mac"
rm -rf "$CHECK"; mkdir -p "$CHECK"
ditto -x -k "$ARCHIVE" "$CHECK"

PROBLEMS=""
[ -x "$CHECK/SignalBox/SignalBox" ] || PROBLEMS="$PROBLEMS\n  нет SignalBox/SignalBox (установщик ищет именно его)"
[ -f "$CHECK/SignalBox/libCr_Core.dylib" ] || PROBLEMS="$PROBLEMS\n  нет libCr_Core.dylib"
[ -d "$CHECK/SignalBox/Contents/Frameworks/CrAdapter" ] || PROBLEMS="$PROBLEMS\n  нет Contents/Frameworks/CrAdapter"
[ -f "$CHECK/SignalBox/www/cam-control-panel.html" ] || PROBLEMS="$PROBLEMS\n  нет www/cam-control-panel.html"
[ -d "$CHECK/__MACOSX" ] && PROBLEMS="$PROBLEMS\n  в архиве папка __MACOSX (лишний мусор)"
for bad in cameras.txt cameras-known.txt groups.json settings.json firewall-skip.txt; do
    [ -e "$CHECK/SignalBox/$bad" ] && PROBLEMS="$PROBLEMS\n  в архиве данные установки: $bad"
done

ADAPTERS="$(ls -1 "$CHECK/SignalBox/Contents/Frameworks/CrAdapter" 2>/dev/null | wc -l | tr -d ' ')"
rm -rf "$CHECK"

if [ -n "$PROBLEMS" ]; then
    printf "ОШИБКА: архив собран неправильно:%b\n" "$PROBLEMS" >&2
    exit 1
fi

SIZE="$(du -h "$ARCHIVE" | cut -f1)"
echo
echo "Готово: $ARCHIVE ($SIZE)"
echo "  распаковывается в SignalBox/, адаптеров внутри: $ADAPTERS, панель на месте"
echo
echo "Дальше на GitHub: Releases -> Create a new release"
echo "  1) тег ОБЯЗАН быть версией: v$VERSION (тег вроде Minor обновление не найдёт);"
echo "  2) ВИНДОВЫЙ архив приложить ПЕРВЫМ, mac — вторым: версии по 1.0.5"
echo "     включительно берут первый .zip в релизе и метку ОС не понимают;"
echo "  3) не забыть приложить сам файл — релиз v1.0.3 вышел пустым."
