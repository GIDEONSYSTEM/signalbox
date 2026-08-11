#!/bin/bash
# Установщик SignalBox под macOS: .pkg с лицензией и установкой в «Программы».
#
# Запуск из корня репозитория:  bash tools/pack-mac-pkg.sh
# На выходе:                    dist/SignalBox-<версия>-mac.pkg
#
# ЗАЧЕМ ОН НУЖЕН И ЧЕГО НЕ ДЕЛАЕТ — читать обязательно:
#
#   * .pkg нужен ради ЛИЦЕНЗИИ и нормальной установки в /Applications. Пароль
#     (или Touch ID), который спросит установщик, — это разрешение ПИСАТЬ в
#     /Applications, и ничего больше.
#   * 🔴 Пароль НЕ делает программу «проверенной» для системы. Доверие
#     Gatekeeper определяется подписью, а не способом установки. Неподписанный
#     .pkg сам будет заблокирован ровно так же, как сейчас блокируется .app.
#   * ✅ Польза всё же есть, и не та, о которой думают: файлы, которые кладёт
#     установщик, НЕ помечаются карантином (его ставит браузер на скачанное, а
#     installd payload не метит). То есть разрешить придётся ОДИН раз — сам
#     .pkg, а установленная программа дальше запускается без вопросов.
#     ⚠️ Это поведение проверено на структуре пакета, но не на чистой машине:
#     подтвердить может только чужой Mac (см. §14).
#   * Полностью без единого нажатия — только с Developer ID: подписать бандл
#     сертификатом Application, пакет — сертификатом Installer, и нотаризовать
#     оба. Тогда ключ SIGNALBOX_PKG_IDENTITY ниже.
#
# .pkg — это ПЕРВАЯ установка. Обновления по-прежнему ходят zip-архивом
# (tools/pack-mac.sh): механизм обновления заменяет бандл на месте и про .pkg
# ничего не знает. Выбор архива смотрит только на .zip, поэтому .pkg в релизе
# ему не мешает (в тесте это случай «dmg + zip»).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build-mac"
DIST="$REPO/dist"
APP="$BUILD/SignalBox.app"

# Сертификат для подписи ПАКЕТА — это "Developer ID Installer", отдельный от
# того, которым подписывается бандл ("Developer ID Application"). Пусто = без
# подписи.
PKG_IDENTITY="${SIGNALBOX_PKG_IDENTITY:-}"

BUNDLE_ID="com.gideonsystem.signalbox"

cd "$REPO"

VERSION="$(sed -n 's/.*kAppVersion *= *"\([^"]*\)".*/\1/p' src/main.cpp | head -1)"
[ -n "$VERSION" ] || { echo "ОШИБКА: не нашёл kAppVersion в src/main.cpp" >&2; exit 1; }

MIN_OS="$(sed -n 's/.*CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9.]*\)".*/\1/p' CMakeLists.txt | head -1)"
[ -n "$MIN_OS" ] || MIN_OS="12.1"

echo "SignalBox $VERSION — сборка установщика .pkg (минимум macOS $MIN_OS)"

if [ ! -x "$APP/Contents/MacOS/SignalBox" ]; then
    echo "ОШИБКА: нет собранного бандла $APP — сначала собери:" >&2
    echo "  cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release && cmake --build build-mac" >&2
    exit 1
fi

PLIST_VER="$(defaults read "$APP/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo "")"
if [ "$PLIST_VER" != "$VERSION" ]; then
    echo "ОШИБКА: в Info.plist версия $PLIST_VER, а в main.cpp $VERSION — пересобери" >&2
    exit 1
fi

WORK="$DIST/pkg-work"
ROOT="$WORK/root"                 # что окажется внутри /Applications
RES="$WORK/resources"             # лицензия и финальный экран
COMPONENT="$WORK/SignalBox-component.pkg"
PKG="$DIST/SignalBox-$VERSION-mac.pkg"

rm -rf "$WORK"
mkdir -p "$ROOT" "$RES" "$DIST"

echo "  копирую бандл"
ditto "$APP" "$ROOT/SignalBox.app"
# Тот же /usr/bin/xattr, что и в pack-mac.sh: в PATH может стоять питоновский
# пакет с тем же именем, который -r не понимает (см. §14).
/usr/bin/xattr -cr "$ROOT" 2>/dev/null || true

echo "  готовлю лицензию и финальный экран"
cp res/license.txt    "$RES/license.txt"
cp res/conclusion.txt "$RES/conclusion.txt"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@MIN_OS@/$MIN_OS/g" \
    res/distribution.xml.in > "$WORK/distribution.xml"

# --- компонент ---
# 🔴 BundleIsRelocatable=false обязателен. По умолчанию установщик считает
# приложение «перемещаемым»: если у пользователя уже лежит SignalBox.app где-то
# ещё (например, распакованный из zip в Загрузках), пакет обновит ТУ копию, а в
# /Applications не появится ничего. Пользователь при этом видит «установка
# завершена». Классическая ловушка pkgbuild.
echo "  собираю компонент"
pkgbuild --analyze --root "$ROOT" "$WORK/component.plist" >/dev/null
plutil -replace 0.BundleIsRelocatable -bool false "$WORK/component.plist"

pkgbuild --root "$ROOT" \
         --component-plist "$WORK/component.plist" \
         --identifier "$BUNDLE_ID" \
         --version "$VERSION" \
         --install-location /Applications \
         "$COMPONENT" >/dev/null

# --- итоговый пакет с окном установщика ---
echo "  собираю установщик"
rm -f "$PKG"
productbuild --distribution "$WORK/distribution.xml" \
             --resources "$RES" \
             --package-path "$WORK" \
             "$PKG" >/dev/null

if [ -n "$PKG_IDENTITY" ]; then
    echo "  подписываю пакет: $PKG_IDENTITY"
    productsign --sign "$PKG_IDENTITY" "$PKG" "$PKG.signed"
    mv "$PKG.signed" "$PKG"
fi

# --- самопроверка: разобрать готовый пакет и убедиться, что внутри то самое ---
echo "  проверяю пакет разбором"
CHECK="$DIST/pkg-check"
rm -rf "$CHECK"
pkgutil --expand-full "$PKG" "$CHECK"

PROBLEMS=""
PAYLOAD="$CHECK/SignalBox-component.pkg/Payload"
[ -x "$PAYLOAD/SignalBox.app/Contents/MacOS/SignalBox" ] \
    || PROBLEMS="$PROBLEMS\n  в пакете нет SignalBox.app/Contents/MacOS/SignalBox"
[ -d "$PAYLOAD/SignalBox.app/Contents/Frameworks/CrAdapter" ] \
    || PROBLEMS="$PROBLEMS\n  нет Frameworks/CrAdapter (без него камеры не находятся)"
[ -f "$PAYLOAD/SignalBox.app/Contents/Resources/www/cam-control-panel.html" ] \
    || PROBLEMS="$PROBLEMS\n  нет панели www/"
grep -q "license.txt" "$CHECK/Distribution" \
    || PROBLEMS="$PROBLEMS\n  в сценарии установщика не подключена лицензия"
[ -f "$CHECK/Resources/license.txt" ] \
    || PROBLEMS="$PROBLEMS\n  файла лицензии нет в ресурсах пакета"
grep -q "install-location=\"/Applications\"" "$CHECK/SignalBox-component.pkg/PackageInfo" \
    || PROBLEMS="$PROBLEMS\n  пакет ставит НЕ в /Applications"
# Перемещаемость видна по СОДЕРЖИМОМУ блока <relocate> в PackageInfo: пустой
# <relocate/> значит «перемещать нечего», а перечисленный внутри бандл — что
# установщик пойдёт искать уже лежащую у пользователя копию.
# ⚠️ Именно по содержимому, а не по атрибуту relocatable="false": он стоит и в
# пакете, собранном БЕЗ запрета, — проверено сборкой заведомо плохого пакета,
# поэтому проверять его бессмысленно. Здешний вариант на том плохом пакете
# срабатывает, то есть это настоящая проверка, а не украшение.
RELOC="$(tr -d '\n' < "$CHECK/SignalBox-component.pkg/PackageInfo")"
case "$RELOC" in
    *'<relocate>'*'<bundle'*)
        PROBLEMS="$PROBLEMS\n  в <relocate> перечислен бандл: установщик обновит уже лежащую у пользователя копию вместо /Applications" ;;
esac
codesign --verify --deep --strict "$PAYLOAD/SignalBox.app" 2>/dev/null \
    || PROBLEMS="$PROBLEMS\n  подпись бандла внутри пакета не прошла проверку"

# || true обязателен: на НЕподписанном пакете pkgutil выходит с ненулевым кодом,
# и при set -e + pipefail скрипт умер бы молча ровно перед тем, как рассказать,
# что не так с пакетом. Тот же капкан уже ловил ls в pack-mac.sh.
SIGN_STATE="$(pkgutil --check-signature "$PKG" 2>&1 | sed -n 's/^   Status: //p' | head -1 || true)"
rm -rf "$CHECK" "$WORK"

if [ -n "$PROBLEMS" ]; then
    printf "ОШИБКА: пакет собран неправильно:%b\n" "$PROBLEMS" >&2
    exit 1
fi

echo
echo "Готово: $PKG ($(du -h "$PKG" | cut -f1))"
echo "  ставит в /Applications, лицензия подключена, подпись пакета: ${SIGN_STATE:-нет}"
if [ -z "$PKG_IDENTITY" ]; then
    echo "  ⚠️  пакет НЕ подписан: на чужом Mac Gatekeeper заблокирует сам установщик,"
    echo "      и его придётся один раз разрешить в Системных настройках."
    echo "      Убрать и это нажатие: SIGNALBOX_PKG_IDENTITY=\"Developer ID Installer: ...\""
    echo "      плюс нотаризация (см. шапку скрипта)."
fi
echo
echo "Проверить установщик, не устанавливая:  open \"$PKG\""
echo "Текст лицензии правится в res/license.txt, финальный экран — res/conclusion.txt."
