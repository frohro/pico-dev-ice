########################################################################
# SoapySDR driver for the Pico-Dev-iCE DDC SDR & WWU 2026 SDR board
# Requires: SoapySDR >= 0.8, a C++14 compiler
# Audio:  miniaudio.h (vendored single-header, MIT)
# Serial: OS APIs directly (termios on POSIX, Win32 on Windows)
########################################################################
cmake_minimum_required(VERSION 3.10)
project(Soapy2026SDR CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

########################################################################
# SoapySDR
########################################################################
find_package(SoapySDR "0.8.0" NO_MODULE REQUIRED)
if(NOT SoapySDR_FOUND)
    message(FATAL_ERROR "SoapySDR development files not found")
endif()

########################################################################
# Platform-specific libraries
########################################################################
set(PLATFORM_LIBS)

if(WIN32)
    # miniaudio needs ole32/winmm; serial uses setupapi/advapi32
    list(APPEND PLATFORM_LIBS ole32 winmm setupapi advapi32)
elseif(APPLE)
    find_library(COREAUDIO_LIB   CoreAudio   REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(COREFOUND_LIB   CoreFoundation REQUIRED)
    find_library(IOKIT_LIB       IOKit        REQUIRED)
    list(APPEND PLATFORM_LIBS
        ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB}
        ${COREFOUND_LIB} ${IOKIT_LIB})
else()  # Linux
    list(APPEND PLATFORM_LIBS dl m pthread)
    # libudev for USB VID/PID enumeration (optional but recommended)
    find_library(UDEV_LIB udev)
    if(UDEV_LIB)
        list(APPEND PLATFORM_LIBS ${UDEV_LIB})
        add_definitions(-DHAVE_LIBUDEV)
    endif()
endif()

########################################################################
# Driver module
########################################################################
SOAPY_SDR_MODULE_UTIL(
    TARGET  2026sdrSupport
    SOURCES
        Registration.cpp
        Settings.cpp
        Streaming.cpp
        SerialPort.cpp
    LIBRARIES
        ${PLATFORM_LIBS}
)

target_include_directories(2026sdrSupport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Silence miniaudio's (harmless) unused-parameter warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(2026sdrSupport PRIVATE -Wno-unused-parameter)
endif()

message(STATUS "Soapy2026SDR: will install to ${CMAKE_INSTALL_PREFIX}")
