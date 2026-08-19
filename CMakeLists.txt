cmake_minimum_required(VERSION 3.15)

# Планка macOS взята из сэмпла Sony — SDK собран под неё.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.1" CACHE STRING "Minimum macOS deployment version")
endif()

project(SignalBox LANGUAGES CXX)

# Чем подписывать .app-бандл. "-" — подпись ad-hoc: бандл собран правильно и
# работает на своей машине, но для раздачи нужен Developer ID (см. §14).
set(SIGNALBOX_CODESIGN_IDENTITY "-" CACHE STRING
    "Сертификат для codesign (\"-\" = ad-hoc; для релиза Developer ID Application: ...)")

# Платформенный слой под macOS написан на Objective-C++ (AppKit, CoreMIDI).
if(APPLE)
    enable_language(OBJCXX)
endif()

# Reuse the Sony Camera Remote SDK shipped with the RemoteCli sample (sibling dir).
set(REMOTECLI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../RemoteCli")
set(CRSDK_INC     "${REMOTECLI_DIR}/app/CRSDK")
set(CRSDK_LIBDIR  "${REMOTECLI_DIR}/external/crsdk")

if(NOT EXISTS "${CRSDK_INC}/CameraRemote_SDK.h")
    message(FATAL_ERROR "CRSDK headers not found at ${CRSDK_INC}. "
                        "Expected SignalBox next to RemoteCli.")
endif()

# Платформенный слой: интерфейс общий, реализация — по одной на ОС.
# Порт на macOS = добавить сюда PlatformMac.mm, остальной код не трогать.
if(WIN32)
    set(PLATFORM_SOURCES
        src/platform/PlatformWin.cpp
        res/app.rc)       # значок приложения — ресурс Windows
elseif(APPLE)
    set(PLATFORM_SOURCES src/platform/PlatformMac.mm)
else()
    message(FATAL_ERROR "Нет реализации платформенного слоя для этой ОС (см. src/platform/Platform.h)")
endif()

# Версию держим в одном месте — kAppVersion в main.cpp. Info.plist берёт её
# отсюда же, иначе «About» бандла и проверка обновлений разойдутся молча.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" _main_src)
string(REGEX MATCH "kAppVersion *= *\"([0-9._]+)\"" _m "${_main_src}")
set(SIGNALBOX_VERSION "${CMAKE_MATCH_1}")
if(NOT SIGNALBOX_VERSION)
    message(FATAL_ERROR "Не нашёл kAppVersion в src/main.cpp")
endif()
unset(_main_src)
# 🔴 Без этой строки CMake не знает, что зависит от main.cpp: правка kAppVersion
# запускает только пересборку, а конфигурация остаётся старой — и Info.plist
# бандла хранит прежнюю версию, из-за чего pack-mac.sh падает с «пересобери»,
# хотя пересборка как раз не помогает (нужна реконфигурация). Проверено:
# без CONFIGURE_DEPENDS смена 1.0.5 -> 1.0.6 не перезапустила configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
message(STATUS "SignalBox версия ${SIGNALBOX_VERSION}")

add_executable(SignalBox
    src/main.cpp
    src/CameraSession.cpp
    src/BmdCamera.cpp
    src/CameraSession.h
    src/HttpServer.h
    src/UpdateAsset.h
    src/platform/Platform.h
    ${PLATFORM_SOURCES}
)

set_target_properties(SignalBox PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(WIN32)
    set_target_properties(SignalBox PROPERTIES
        WIN32_EXECUTABLE TRUE      # /SUBSYSTEM:WINDOWS -> no console window at all
    )
endif()

if(APPLE)
    # Info.plist собирается из шаблона: туда подставляется версия и планка ОС.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/Info.plist.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

    set_target_properties(SignalBox PROPERTIES
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED YES
        # 🔴 Настоящий .app-бандл, а не голый бинарник. Зачем:
        #   * разрешение «Локальная сеть» macOS выдаёт приложению, а у голого
        #     бинарника его получает ТЕРМИНАЛ — на чужой машине это вечное
        #     «камеры не находятся»;
        #   * двойной клик по бинарнику открывал Терминал, по бандлу — запускает;
        #   * подписать и нотаризовать можно только бандл;
        #   * SDK Sony ищет адаптеры в <bundlePath>/Contents/Frameworks/CrAdapter
        #     ([[NSBundle mainBundle] bundlePath] — проверено в libCr_Core),
        #     то есть внутри бандла это ровно штатная раскладка.
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        # Библиотеки Sony уезжают в Contents/Frameworks — туда и смотрим.
        INSTALL_RPATH "@executable_path/../Frameworks"
        # Без этого CMake дописывает в RPATH абсолютный путь к папке SDK на
        # машине сборки. На чужом Mac его нет — в лучшем случае мусор в бинарнике,
        # в худшем библиотека подхватится не та. Собираем сразу с итоговым RPATH.
        BUILD_WITH_INSTALL_RPATH TRUE
    )
    # 🔴 ARC обязателен: без него объект, пойманный из completion-блока
    # NSURLSession в __block-переменную, успевает освободиться до того, как мы
    # его прочитаем (блок отрабатывает в чужом потоке со своим autorelease-пулом).
    set_source_files_properties(src/platform/PlatformMac.mm
                                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

if(MSVC)
    # Keep plain main() as the entry point despite the WINDOWS subsystem
    # (the alternative would be rewriting main as WinMain for no benefit).
    target_link_options(SignalBox PRIVATE /ENTRY:mainCRTStartup)
endif()

target_include_directories(SignalBox PRIVATE ${CRSDK_INC})

# 🔴 UNICODE определяем ТОЛЬКО под Windows — ровно как сэмпл Sony (RemoteCli).
# От этого зависит тип CrChar в CrTypes.h: с UNICODE это wchar_t, без него — char.
# DLL под Windows собраны с UNICODE, а dylib под macOS — без, и определить его
# «за компанию» на Mac значит разойтись с библиотекой по ABI.
if(WIN32)
    target_compile_definitions(SignalBox PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    # /utf-8 so the Russian console strings compile correctly.
    # 4819/4828: SDK headers carry non-UTF-8 (cp932) bytes in comments -> harmless.
    target_compile_options(SignalBox PRIVATE /utf-8 /W3 /wd4819 /wd4828 /guard:cf)
endif()

find_library(CR_CORE_LIB Cr_Core HINTS ${CRSDK_LIBDIR})
if(NOT CR_CORE_LIB)
    message(FATAL_ERROR "Cr_Core.lib not found in ${CRSDK_LIBDIR}")
endif()

target_link_libraries(SignalBox PRIVATE ${CR_CORE_LIB})
if(WIN32)
    # Всё это тянет ТОЛЬКО платформенный слой Windows (src/platform/PlatformWin.cpp).
    target_link_libraries(SignalBox PRIVATE ws2_32 winmm shell32 iphlpapi ole32 oleaut32 winhttp)
endif()

if(APPLE)
    # Аналогично: фреймворки нужны только src/platform/PlatformMac.mm.
    #   AppKit    — значок в строке меню, NSAlert, открыть ссылку в браузере
    #   Foundation— NSURLSession (HTTP-клиент), пути, имя компьютера
    #   CoreMIDI  — вход с пульта
    find_library(APPKIT_FW     AppKit     REQUIRED)
    find_library(FOUNDATION_FW Foundation REQUIRED)
    find_library(COREMIDI_FW   CoreMIDI   REQUIRED)
    # Carbon — ради RegisterEventHotKey (системные горячие клавиши). API старое и
    # помечено устаревшим, но замены для «зарегистрировать комбинацию, не читая
    # чужой ввод» у Apple нет: всё современное (CGEventTap, NSEvent monitor)
    # требует разрешения «Мониторинг ввода» и видит вообще всё нажатое.
    # Без этой строки файл КОМПИЛИРУЕТСЯ, но линковка падает на
    # _RegisterEventHotKey / _GetApplicationEventTarget / _InstallEventHandler.
    find_library(CARBON_FW     Carbon     REQUIRED)
    target_link_libraries(SignalBox PRIVATE
        ${APPKIT_FW} ${FOUNDATION_FW} ${COREMIDI_FW} ${CARBON_FW})
endif()

if(WIN32)
    # Ship the MSVC runtime next to the exe (app-local deployment) so SignalBox starts on a
    # clean PC with no Visual C++ Redistributable installed. Needed not just by our exe:
    # the Sony SDK DLLs (Cr_Core, Cr_PTP_IP, Cr_PTP_USB, ...) import MSVCP140/VCRUNTIME140 too.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)   # copy manually, don't add an install() rule
    include(InstallRequiredSystemLibraries)

    # After build: drop SDK runtime DLLs + CrAdapter/ next to the exe, ensure www/ exists.
    add_custom_command(TARGET SignalBox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CRSDK_LIBDIR}" "$<TARGET_FILE_DIR:SignalBox>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:SignalBox>/www"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "$<TARGET_FILE_DIR:SignalBox>"
        VERBATIM
    )
endif()

if(APPLE)
    # Раскладка бандла:
    #   Contents/MacOS/SignalBox            — сам бинарник ($<TARGET_FILE_DIR:>)
    #   Contents/Frameworks/*.dylib         — библиотеки Sony (туда смотрит RPATH)
    #   Contents/Frameworks/CrAdapter/      — 🔴 адаптеры SDK; ищутся именно здесь,
    #                                         иначе камеры не находятся вообще
    #   Contents/Resources/www/             — панель
    #   Contents/Resources/icon.icns        — значок (res/icon.ico -> tools/make-icns.sh)
    set(BUNDLE_CONTENTS "$<TARGET_BUNDLE_CONTENT_DIR:SignalBox>")

    add_custom_command(TARGET SignalBox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CRSDK_LIBDIR}" "${BUNDLE_CONTENTS}/Frameworks"
        # copy_directory затащил и CrAdapter — он уже на своём месте внутри
        # Frameworks, отдельного переноса (как было у голого бинарника) не нужно.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_CONTENTS}/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.icns"
                "${BUNDLE_CONTENTS}/Resources/icon.icns"
        # Панель исторически лежит в build/Release/www (так она попала под git) —
        # на Windows каталог сборки совпадает, на macOS нет, поэтому копируем явно.
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/build/Release/www"
                "${BUNDLE_CONTENTS}/Resources/www"
        # 🔴 Подписывать ПОСЛЕ того, как всё содержимое на месте. Линковщик
        # подписывает только сам бинарник, и без этого шага бандл собран
        # неправильно: `codesign --verify` говорит «code has no resources but
        # signature indicates they must be present». Для нотаризации это блокер.
        # --deep заодно перекрывает своей подписью dylib Sony (у них своя ad-hoc).
        #
        # По умолчанию подпись ad-hoc ("-"): её хватает, чтобы бандл был
        # правильным и запускался на этой машине, но НЕ хватает для раздачи.
        # Для релиза передать сертификат:
        #   cmake -S . -B build-mac -DSIGNALBOX_CODESIGN_IDENTITY="Developer ID Application: ..."
        COMMAND codesign --force --deep --sign "${SIGNALBOX_CODESIGN_IDENTITY}"
                "$<TARGET_BUNDLE_DIR:SignalBox>"
        VERBATIM
    )
endif()
