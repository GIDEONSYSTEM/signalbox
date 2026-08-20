# Упаковка релизного архива SignalBox под Windows.
#
# Запуск из корня репозитория:   powershell -ExecutionPolicy Bypass -File tools\pack-win.ps1
# На выходе:                     dist\SignalBox-<версия>-win.zip
#
# ⚠️ ФАЙЛ ОБЯЗАН ХРАНИТЬСЯ В UTF-8 С BOM. Windows PowerShell 5.1 без BOM читает
#    скрипт в системной ANSI-кодировке, русский текст превращается в мусор и
#    парсер падает на «Unexpected token» в местах, где ничего не сломано.
#    Редактор, сохраняющий без BOM, ломает скрипт молча.
#
# Что важно и почему:
#   * состав берётся из build\Release, но НЕ целиком: рядом с exe лежат данные
#     конкретной установки и следы сборки, и то и другое в релизе не место;
#   * внутри архива всё лежит в папке SignalBox\ верхним уровнем — установщик
#     обновления ищет ровно это имя (payloadName() в main.cpp);
#   * метка «-win» в имени файла обязательна: по ней приложение выбирает свою
#     сборку из релиза, где лежат обе (см. src/UpdateAsset.h);
#   * 🐞 данные студии (cameras.txt, groups.json, cardlayout.json, ...) в архив попасть не должны:
#     robocopy при обновлении затрёт ими рабочие файлы. §7 на этом уже спотыкался,
#     поэтому здесь и явный список включаемого, и проверка результата;
#   * версия читается из src\main.cpp, чтобы имя архива и тег релиза не разошлись.

$ErrorActionPreference = 'Stop'

$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build\Release'
$dist  = Join-Path $repo 'dist'
$stage = Join-Path $dist 'stage-win'

# --- версия из исходника ---
$mainSrc = Get-Content (Join-Path $repo 'src\main.cpp') -Raw -Encoding UTF8
if ($mainSrc -notmatch 'kAppVersion\s*=\s*"([0-9._]+)"') {
    Write-Error 'не нашёл kAppVersion в src\main.cpp'
}
$version = $Matches[1]
$archive = Join-Path $dist "SignalBox-$version-win.zip"

Write-Host "SignalBox $version — сборка архива под Windows"

$exe = Join-Path $build 'SignalBox.exe'
if (-not (Test-Path $exe)) {
    Write-Error "нет собранного $exe — сначала собери проект (см. §1 документации)"
}

# Собран ли exe ПОСЛЕ последней правки исходников. Иначе легко выпустить релиз
# со старым бинарником и новым номером версии в имени файла.
$newestSrc = Get-ChildItem (Join-Path $repo 'src') -Recurse -File |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSrc.LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    Write-Error "$($newestSrc.Name) новее собранного exe — пересобери проект, иначе в архив уедет старая сборка"
}

# --- чистая площадка ---
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$payload = Join-Path $stage 'SignalBox'
New-Item -ItemType Directory -Force -Path $payload | Out-Null

# Явный список того, что входит. Всё остальное — за бортом по умолчанию:
# так новый файл сборки не уедет в релиз молча.
Write-Host '  копирую программу и библиотеки'
Copy-Item $exe $payload
Get-ChildItem $build -Filter '*.dll' | Copy-Item -Destination $payload
Copy-Item (Join-Path $build 'CrAdapter') $payload -Recurse    # адаптеры SDK: без них камеры не находятся
Copy-Item (Join-Path $build 'www')       $payload -Recurse    # панель

foreach ($extra in 'ЧИТАЙ-МЕНЯ.txt', 'START_SignalBox.bat') {
    $src = Join-Path $build $extra
    if (Test-Path $src) { Write-Host "  кладу $extra"; Copy-Item $src $payload }
}

# --- проверка состава ДО упаковки ---
Write-Host '  проверяю, что данных установки и следов сборки внутри нет'
$forbidden = @()
foreach ($bad in 'cameras.txt','cameras-known.txt','groups.json','settings.json','cardlayout.json','firewall-skip.txt') {
    if (Get-ChildItem $payload -Recurse -Filter $bad -ErrorAction SilentlyContinue) { $forbidden += $bad }
}
# .lnk — абсолютные пути этой машины, на чужой он битый; .lib/.pdb/.log — не для релиза.
foreach ($mask in '*.log','*.log.prev','*.lnk','*.lib','*.exp','*.pdb','*.ilk') {
    $hit = Get-ChildItem $payload -Recurse -Filter $mask -ErrorAction SilentlyContinue
    if ($hit) { $forbidden += $mask }
}
if ($forbidden.Count) {
    Write-Error "в архив попало лишнее: $($forbidden -join ', ')"
}

# --- упаковка ---
Write-Host "  пакую $archive"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $archive) { Remove-Item $archive -Force }
Compress-Archive -Path $payload -DestinationPath $archive -CompressionLevel Optimal

# --- самопроверка: распаковать ТЕМ ЖЕ инструментом, что и установщик ---
# Обновление распаковывает архив через tar.exe (plat::extractArchive). Проверяем
# именно им: минута здесь дешевле сломанного обновления у студий (§7, дважды).
Write-Host '  проверяю архив распаковкой через tar.exe — как это сделает обновление'
$check = Join-Path $dist 'check-win'
if (Test-Path $check) { Remove-Item $check -Recurse -Force }
New-Item -ItemType Directory -Force -Path $check | Out-Null
& tar.exe -xf $archive -C $check
if ($LASTEXITCODE -ne 0) { Write-Error "tar.exe не смог распаковать архив (код $LASTEXITCODE)" }

$root = Join-Path $check 'SignalBox'
$problems = @()
foreach ($must in 'SignalBox.exe','Cr_Core.dll','vcruntime140.dll','msvcp140.dll',
                  'CrAdapter\Cr_PTP_IP.dll','www\cam-control-panel.html','www\qrcode.js') {
    if (-not (Test-Path (Join-Path $root $must))) { $problems += "нет $must" }
}
$adapters = (Get-ChildItem (Join-Path $root 'CrAdapter') -File -ErrorAction SilentlyContinue).Count
$fonts    = (Get-ChildItem (Join-Path $root 'www\fonts') -File -ErrorAction SilentlyContinue).Count
if ($fonts -lt 1) { $problems += 'нет шрифтов в www\fonts (панель уйдёт за ними в интернет)' }

Remove-Item $check -Recurse -Force
Remove-Item $stage -Recurse -Force

if ($problems.Count) {
    Write-Error "архив собран неправильно:`n  $($problems -join "`n  ")"
}

$sizeMb = [math]::Round((Get-Item $archive).Length / 1MB, 1)
Write-Host ''
Write-Host "Готово: $archive ($sizeMb МБ)"
Write-Host "  адаптеров внутри: $adapters, шрифтов панели: $fonts"
Write-Host ''
Write-Host 'Дальше на GitHub: Releases -> Create a new release'
Write-Host "  1) тег ОБЯЗАН быть версией: v$version (тег вроде Minor обновление не найдёт);"
Write-Host '  2) приложить оба архива, win и mac — порядок загрузки роли не играет:'
Write-Host '     GitHub всё равно отдаёт вложения отсортированными по имени файла;'
Write-Host '  3) не забыть приложить сам файл — релиз v1.0.3 вышел пустым.'
