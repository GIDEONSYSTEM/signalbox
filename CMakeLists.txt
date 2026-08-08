cmake_minimum_required(VERSION 3.15)
project(SignalBox LANGUAGES CXX)

# Reuse the Sony Camera Remote SDK shipped with the RemoteCli sample (sibling dir).
set(REMOTECLI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../RemoteCli")
set(CRSDK_INC     "${REMOTECLI_DIR}/app/CRSDK")
set(CRSDK_LIBDIR  "${REMOTECLI_DIR}/external/crsdk")

if(NOT EXISTS "${CRSDK_INC}/CameraRemote_SDK.h")
    message(FATAL_ERROR "CRSDK headers not found at ${CRSDK_INC}. "
                        "Expected SignalBox next to RemoteCli.")
endif()

add_executable(SignalBox
    src/main.cpp
    src/CameraSession.cpp
    src/CameraSession.h
    src/HttpServer.h
    res/app.rc            # embeds the Wi-Fi application icon
)

set_target_properties(SignalBox PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    WIN32_EXECUTABLE TRUE          # /SUBSYSTEM:WINDOWS -> no console window at all
)

if(MSVC)
    # Keep plain main() as the entry point despite the WINDOWS subsystem
    # (the alternative would be rewriting main as WinMain for no benefit).
    target_link_options(SignalBox PRIVATE /ENTRY:mainCRTStartup)
endif()

target_include_directories(SignalBox PRIVATE ${CRSDK_INC})

# SDK DLLs are built UNICODE -> CrChar must be wchar_t in our TU too.
target_compile_definitions(SignalBox PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    # /utf-8 so the Russian console strings compile correctly.
    # 4819/4828: SDK headers carry non-UTF-8 (cp932) bytes in comments -> harmless.
    target_compile_options(SignalBox PRIVATE /utf-8 /W3 /wd4819 /wd4828 /guard:cf)
endif()

find_library(CR_CORE_LIB Cr_Core HINTS ${CRSDK_LIBDIR})
if(NOT CR_CORE_LIB)
    message(FATAL_ERROR "Cr_Core.lib not found in ${CRSDK_LIBDIR}")
endif()

target_link_libraries(SignalBox PRIVATE ${CR_CORE_LIB} ws2_32 winmm shell32 iphlpapi ole32 oleaut32 winhttp)

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
