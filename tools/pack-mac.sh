#!/bin/bash
# Упаковка релизного архива SignalBox под macOS.
#
# Запуск из корня репозитория:   bash tools/pack-mac.sh
# На выходе:                     dist/SignalBox-<версия>-mac.zip
#
# Что важно и почему:
#   * пакуется .app-бандл ЦЕЛИКОМ — он и есть единица установки: программа,
#     библиотеки Sony, адаптеры и панель уже внутри, собирать состав руками
#     больше не нужно (этим занимается CMake);
#   * внутри архива бандл лежит верхним уровнем как SignalBox.app — установщик
#     обновления ищет ровно это имя (payloadName() в main.cpp);
#   * метка «-mac» в имени файла обязательна: по ней приложение выбирает свою
#     сборку из релиза, где лежат обе (см. src/UpdateAsset.h);
#   * данные студии (cameras.txt, groups.json, ...) внутрь попасть не могут в
#     принципе: с переходом на бандл они живут в ~/Library/Application Support/
#     SignalBox. Проверка на них всё равно осталась — дешевле, чем повторить
#     историю §7, где первый архив уехал с cameras.txt внутри;
#   * карантин снимается: dylib Sony приезжают из загруженного браузером пакета
#     SDK вместе с com.apple.quarantine, и этот атрибут переживает копирование.
#
# ⚠️ Бандл подписан ad-hoc — этого хватает, чтобы он был правильно собран и
#    работал на своей машине, но НЕ хватает для раздачи: на чужом Mac Gatekeeper
#    будет ругаться. Для релиза пересобрать с сертификатом и нотаризовать:
#      cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release \
#            -DSIGNALBOX_CODESIGN_IDENTITY="Developer ID Application: ИМЯ (TEAMID)"
#      cmake --build build-mac && bash tools/pack-mac.sh
#      xcrun notarytool submit dist/SignalBox-<в>-mac.zip --apple-id ... --wait
#      xcrun stapler staple build-mac/SignalBox.app   # затем упаковать заново

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build-mac"
DIST="$REPO/dist"
APP="$BUILD/SignalBox.app"

cd "$REPO"

# --- версия берётся из исходника, чтобы имя архива и тег релиза не разошлись ---
VERSION="$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' src/main.cpp | head -1)"
if [ -z "$VERSION" ]; then
    echo "ОШИБКА: не нашёл kAppVersion в src/main.cpp" >&2
    exit 1
fi

STAGE="$DIST/stage-mac"
ARCHIVE="$DIST/SignalBox-$VERSION-mac.zip"

echo "SignalBox $VERSION — сборка архива под macOS"

if [ ! -x "$APP/Contents/MacOS/SignalBox" ]; then
    echo "ОШИБКА: нет собранного бандла $APP — сначала собери:" >&2
    echo "  cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release && cmake --build build-mac" >&2
    exit 1
fi

# Версия в Info.plist должна совпадать с kAppVersion, иначе «О программе» и
# проверка обновлений разойдутся. CMake подставляет её сам — тут только сверка.
PLIST_VER="$(defaults read "$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo "")"
if [ "$PLIST_VER" != "$VERSION" ]; then
    echo "ОШИБКА: в Info.plist версия $PLIST_VER, а в main.cpp $VERSION — пересобери" >&2
    exit 1
fi

# Архив собираем из свежего каталога, иначе в него незаметно переезжает мусор
# прошлой упаковки.
rm -rf "$STAGE"
mkdir -p "$STAGE"

# ditto, а не cp: сохраняет права, симлинки и подпись бандла в целости.
echo "  копирую бандл"
ditto "$APP" "$STAGE/SignalBox.app"

# Инструкция для коллеги кладётся РЯДОМ с бандлом: внутрь нельзя, любая правка
# содержимого ломает подпись.
for extra in "ЧИТАЙ-МЕНЯ.txt" "README.txt"; do
    if [ -f "$REPO/build/Release/$extra" ]; then
        echo "  кладу $extra рядом с бандлом"
        cp "$REPO/build/Release/$extra" "$STAGE/"
    fi
done

echo "  проверяю, что данных установки внутри нет"
LEAKED=""
for bad in cameras.txt cameras-known.txt groups.json settings.json firewall-skip.txt; do
    if find "$STAGE/SignalBox.app" -name "$bad" | grep -q .; then LEAKED="$LEAKED $bad"; fi
done
if find "$STAGE/SignalBox.app" -name '*.log' | grep -q .; then LEAKED="$LEAKED логи"; fi
if [ -n "$LEAKED" ]; then
    echo "ОШИБКА: в бандл попали данные установки:$LEAKED" >&2
    exit 1
fi

# 🔴 Именно /usr/bin/xattr, а не просто xattr. На машине владельца в PATH стоит
# питоновский пакет xattr (~/Library/Python/3.9/bin/xattr), который перекрывает
# системный и рекурсивного -r НЕ понимает. Из-за этого в проекте дважды решили,
# что «xattr -dr карантин не снимает»: на самом деле команда просто падала.
# Системный умеет -r прекрасно — проверено на 38 файлах, стало 0.
# Подпись живёт в файлах бандла, а не в атрибутах, поэтому чистка её не ломает —
# это подтверждает codesign в самопроверке ниже.
echo "  снимаю карантин (com.apple.quarantine приезжает вместе с dylib Sony)"
/usr/bin/xattr -cr "$STAGE" 2>/dev/null || true

# ditto, а не zip: родной инструмент macOS, сохраняет права, симлинки и
# структуру бандла, и ровно им же приложение архив распаковывает
# (plat::extractArchive).
# ℹ️ Файлы «._имя» внутри архива — норма: xattr -c не снимает
# com.apple.provenance (системный атрибут), и ditto хранит его рядом с файлом.
# При распаковке ditto и Архиватором macOS они склеиваются обратно.
# Без --keepParent: пакуем СОДЕРЖИМОЕ staging, чтобы SignalBox.app лежал верхним
# уровнем архива. С --keepParent ditto завернул бы всё в лишнюю папку stage-mac,
# и установщик не нашёл бы бандл (поймано самопроверкой ниже).
echo "  пакую $ARCHIVE"
rm -f "$ARCHIVE"
mkdir -p "$DIST"
ditto -c -k "$STAGE" "$ARCHIVE"

# --- самопроверка: распаковать так же, как это сделает установщик, и убедиться,
# что раскладка та, которую он ждёт, а подпись пережила упаковку. Дешевле минуты
# здесь, чем сломанное обновление у студий: §7 спотыкался уже дважды.
echo "  проверяю архив распаковкой"
CHECK="$DIST/check-mac"
rm -rf "$CHECK"; mkdir -p "$CHECK"
ditto -x -k "$ARCHIVE" "$CHECK"

A="$CHECK/SignalBox.app"
PROBLEMS=""
[ -x "$A/Contents/MacOS/SignalBox" ]              || PROBLEMS="$PROBLEMS\n  нет SignalBox.app/Contents/MacOS/SignalBox"
[ -f "$A/Contents/Info.plist" ]                   || PROBLEMS="$PROBLEMS\n  нет Info.plist"
[ -f "$A/Contents/Frameworks/libCr_Core.dylib" ]  || PROBLEMS="$PROBLEMS\n  нет Frameworks/libCr_Core.dylib"
[ -d "$A/Contents/Frameworks/CrAdapter" ]         || PROBLEMS="$PROBLEMS\n  нет Frameworks/CrAdapter (без него камеры не находятся)"
[ -f "$A/Contents/Resources/www/cam-control-panel.html" ] || PROBLEMS="$PROBLEMS\n  нет Resources/www/cam-control-panel.html"
[ -f "$A/Contents/Resources/icon.icns" ]          || PROBLEMS="$PROBLEMS\n  нет Resources/icon.icns"
[ -d "$CHECK/__MACOSX" ]                          && PROBLEMS="$PROBLEMS\n  в архиве папка __MACOSX (лишний мусор)"
codesign --verify --deep --strict "$A" 2>/dev/null || PROBLEMS="$PROBLEMS\n  подпись бандла не прошла проверку после упаковки"

# || true обязателен: при set -e + pipefail упавший ls убил бы скрипт молча,
# ровно там, где он собирался напечатать, ЧТО именно не так с архивом.
ADAPTERS="$(ls -1 "$A/Contents/Frameworks/CrAdapter" 2>/dev/null | wc -l | tr -d ' ' || true)"
SIGN="$(codesign -dv "$A" 2>&1 | sed -n 's/^Signature=//p' || true)"
rm -rf "$CHECK" "$STAGE"

if [ -n "$PROBLEMS" ]; then
    printf "ОШИБКА: архив собран неправильно:%b\n" "$PROBLEMS" >&2
    exit 1
fi

SIZE="$(du -h "$ARCHIVE" | cut -f1)"
echo
echo "Готово: $ARCHIVE ($SIZE)"
echo "  бандл распаковывается целиком, адаптеров внутри: $ADAPTERS, подпись: ${SIGN:-неизвестна}"
if [ "$SIGN" = "adhoc" ]; then
    echo "  ⚠️  подпись ad-hoc — на чужом Mac Gatekeeper будет ругаться."
    echo "      Для раздачи пересобрать с -DSIGNALBOX_CODESIGN_IDENTITY и нотаризовать (см. шапку скрипта)."
fi
echo
echo "Дальше на GitHub: Releases -> Create a new release"
echo "  1) тег ОБЯЗАН быть версией: v$VERSION (тег вроде Minor обновление не найдёт);"
echo "  2) ВИНДОВЫЙ архив приложить ПЕРВЫМ, mac — вторым: версии по 1.0.5"
echo "     включительно берут первый .zip в релизе и метку ОС не понимают;"
echo "  3) не забыть приложить сам файл — релиз v1.0.3 вышел пустым."
