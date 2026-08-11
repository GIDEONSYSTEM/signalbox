cmake_minimum_required(VERSION 3.15)

# Планка macOS взята из сэмпла Sony — SDK собран под неё.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.1" CACHE STRING "Minimum macOS deployment version")
endif()

project(SignalBox LANGUAGES CXX)

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

add_executable(SignalBox
    src/main.cpp
    src/CameraSession.cpp
    src/CameraSession.h
    src/HttpServer.h
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
    set_target_properties(SignalBox PROPERTIES
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED YES
        # Библиотеки Sony лежат рядом с бинарником, как и на Windows.
        BUILD_RPATH   "@executable_path"
        INSTALL_RPATH "@executable_path"
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
    target_link_libraries(SignalBox PRIVATE ${APPKIT_FW} ${FOUNDATION_FW} ${COREMIDI_FW})
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
    # 🔴 Адаптеры SDK на macOS ищутся НЕ рядом с бинарником, а в
    # Contents/Frameworks/CrAdapter — так это делает сэмпл Sony. Кладём библиотеки
    # рядом, а папку адаптеров переносим туда, иначе камеры не находятся вообще.
    add_custom_command(TARGET SignalBox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CRSDK_LIBDIR}" "$<TARGET_FILE_DIR:SignalBox>"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:SignalBox>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_FILE_DIR:SignalBox>/CrAdapter"
                "$<TARGET_FILE_DIR:SignalBox>/Contents/Frameworks/CrAdapter"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:SignalBox>/CrAdapter"
        # Панель исторически лежит в build/Release/www (так она попала под git) —
        # на Windows каталог сборки совпадает, на macOS нет, поэтому копируем явно.
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/build/Release/www"
                "$<TARGET_FILE_DIR:SignalBox>/www"
        VERBATIM
    )
endif()
