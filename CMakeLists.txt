# Copyright (C) 2026 AutoGavy
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.28)
project(RegionLockNative VERSION 1.0.0 LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "RegionLock requires Windows and the Microsoft C/C++ toolchain.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0A00)
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>"
    "$<$<COMPILE_LANGUAGE:CXX>:/W4>"
    "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
    "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>"
    "$<$<COMPILE_LANGUAGE:C>:/W3>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/O2>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/GL>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gy>"
    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gw>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf>"
)
add_link_options(
    /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
    "$<$<CONFIG:Release>:/LTCG>"
    "$<$<CONFIG:Release>:/OPT:REF>"
    "$<$<CONFIG:Release>:/OPT:ICF>"
)

add_library(yyjson STATIC third_party/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC third_party/yyjson)

add_library(RegionLockCore STATIC src/Core.cpp)
target_include_directories(RegionLockCore PUBLIC src)
target_link_libraries(RegionLockCore PUBLIC yyjson ws2_32)

set(REGIONLOCK_GUI_SOURCES src/Main.cpp src/Platform.cpp resources/App.rc)
set(REGIONLOCK_GUI_LIBRARIES
    RegionLockCore comctl32 comdlg32 user32 gdi32 shell32 ole32 oleaut32 uuid
    winhttp ws2_32 advapi32 uxtheme dwmapi psapi gdiplus
)

add_executable(RegionLock WIN32 ${REGIONLOCK_GUI_SOURCES})
target_include_directories(RegionLock PRIVATE resources)
target_link_libraries(RegionLock PRIVATE ${REGIONLOCK_GUI_LIBRARIES})
target_link_options(RegionLock PRIVATE /MANIFEST:NO)

# This target exercises UI lifecycle and idle cost without requesting elevation
# or allowing the smoke run to write firewall rules.
add_executable(RegionLockSmoke WIN32 ${REGIONLOCK_GUI_SOURCES})
target_include_directories(RegionLockSmoke PRIVATE resources)
target_compile_definitions(RegionLockSmoke PRIVATE REGIONLOCK_SMOKE=1)
target_link_libraries(RegionLockSmoke PRIVATE ${REGIONLOCK_GUI_LIBRARIES})
target_link_options(RegionLockSmoke PRIVATE /MANIFEST:NO)

include(CTest)
if(BUILD_TESTING)
    add_executable(CoreTests tests/CoreTests.cpp)
    target_link_libraries(CoreTests PRIVATE RegionLockCore)
    add_test(NAME RegionLockCoreTests COMMAND CoreTests "${CMAKE_CURRENT_SOURCE_DIR}/assets/ip-ranges.json")

    add_executable(PlatformTests tests/PlatformTests.cpp src/Platform.cpp)
    target_link_libraries(PlatformTests PRIVATE
        RegionLockCore ole32 oleaut32 uuid winhttp ws2_32 advapi32 shell32
    )
    add_test(NAME RegionLockPlatformTests COMMAND PlatformTests)
endif()
