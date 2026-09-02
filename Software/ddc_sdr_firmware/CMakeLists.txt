cmake_minimum_required(VERSION 3.13)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk/cmake/preinit_pico_ice_sdk.cmake)

if(DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{PICO_SDK_PATH}/pico_sdk_init.cmake")
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk/lib/pico-sdk/pico_sdk_init.cmake")
    set(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk/lib/pico-sdk")
endif()

if(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{PICO_TINYUSB_PATH})
    set(PICO_TINYUSB_PATH "$ENV{PICO_TINYUSB_PATH}" CACHE PATH
        "Path to the TinyUSB checkout used by the DDC firmware")
elseif(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{HOME}
       AND EXISTS "$ENV{HOME}/tinyusb/src/tusb.h"
       AND EXISTS "$ENV{HOME}/tinyusb/hw/bsp/rp2040")
    set(PICO_TINYUSB_PATH "$ENV{HOME}/tinyusb" CACHE PATH
        "Path to the TinyUSB checkout used by the DDC firmware")
elseif(EXISTS "${PICO_SDK_PATH}/lib/tinyusb/src/tusb.h")
    set(PICO_TINYUSB_PATH "${PICO_SDK_PATH}/lib/tinyusb" CACHE PATH
        "Path to the TinyUSB checkout used by the DDC firmware")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ddc_sdr C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()
set(PICO_DEV_ICE 1 CACHE BOOL "Pico Dev-iCE board" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk pico-ice-sdk)

set(FPGA_BITSTREAM_BIN "" CACHE FILEPATH
    "Legacy packed iCE40 bitstream (.bin), used as the stored RX image")
set(FPGA_RX_BITSTREAM_BIN "" CACHE FILEPATH
    "Packed iCE40 RX bitstream (.bin) to embed in the RP2040 firmware")
set(FPGA_TX_BITSTREAM_BIN "" CACHE FILEPATH
    "Packed iCE40 TX bitstream (.bin) to embed in the RP2040 firmware")

# Auto-default to included DDC SDR RX bitstream if available and none specified
if(NOT FPGA_BITSTREAM_BIN AND (NOT FPGA_RX_BITSTREAM_BIN OR FPGA_RX_BITSTREAM_BIN STREQUAL ""))
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bitstreams/ddc_sdr_rx.bin")
        set(FPGA_RX_BITSTREAM_BIN "${CMAKE_CURRENT_SOURCE_DIR}/bitstreams/ddc_sdr_rx.bin" CACHE FILEPATH "RX bitstream" FORCE)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../ENGR433-Solutions/Lab_09/ddc_sdr_top.bin")
        set(FPGA_RX_BITSTREAM_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../../ENGR433-Solutions/Lab_09/ddc_sdr_top.bin" CACHE FILEPATH "RX bitstream" FORCE)
    endif()
endif()

set(_fpga_boot_mode_default "DFU")
if(FPGA_BITSTREAM_BIN OR FPGA_RX_BITSTREAM_BIN OR FPGA_TX_BITSTREAM_BIN)
    set(_fpga_boot_mode_default "STORED")
endif()
set(FPGA_BOOT_MODE "${_fpga_boot_mode_default}" CACHE STRING
    "FPGA boot source: STORED loads an embedded image, DFU waits for dfu-util")
set_property(CACHE FPGA_BOOT_MODE PROPERTY STRINGS STORED DFU)
if(NOT FPGA_BOOT_MODE STREQUAL "STORED" AND NOT FPGA_BOOT_MODE STREQUAL "DFU")
    message(FATAL_ERROR "FPGA_BOOT_MODE must be STORED or DFU")
endif()
set(FPGA_DEFAULT_IMAGE "RX" CACHE STRING
    "Stored FPGA image selected at boot: RX or TX")
set_property(CACHE FPGA_DEFAULT_IMAGE PROPERTY STRINGS RX TX)
if(NOT FPGA_DEFAULT_IMAGE STREQUAL "RX" AND NOT FPGA_DEFAULT_IMAGE STREQUAL "TX")
    message(FATAL_ERROR "FPGA_DEFAULT_IMAGE must be RX or TX")
endif()

if(FPGA_BITSTREAM_BIN AND FPGA_RX_BITSTREAM_BIN AND NOT FPGA_BITSTREAM_BIN STREQUAL FPGA_RX_BITSTREAM_BIN)
    message(FATAL_ERROR
        "Use FPGA_RX_BITSTREAM_BIN instead of the legacy FPGA_BITSTREAM_BIN")
endif()
if(FPGA_BITSTREAM_BIN)
    set(FPGA_RX_BITSTREAM_BIN "${FPGA_BITSTREAM_BIN}")
endif()

set(DDC_GENERATED_INCLUDES)
set(DDC_BITSTREAM_HEADERS)
function(ddc_add_bitstream_header input_file output_file symbol definition)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(header_path ${CMAKE_CURRENT_BINARY_DIR}/${output_file})
    add_custom_command(
        OUTPUT ${header_path}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2header.py
                ${input_file}
                ${header_path}
                ${symbol}
        DEPENDS ${input_file}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2header.py
        VERBATIM
        )
    list(APPEND DDC_BITSTREAM_HEADERS ${header_path})
    set(DDC_BITSTREAM_HEADERS ${DDC_BITSTREAM_HEADERS} PARENT_SCOPE)
    set(${definition} 1 PARENT_SCOPE)
endfunction()

if(FPGA_RX_BITSTREAM_BIN)
    ddc_add_bitstream_header(${FPGA_RX_BITSTREAM_BIN}
        fpga_bitstream_rx.h ddc_fpga_rx_bitstream DDC_HAS_STORED_RX)
endif()
if(FPGA_TX_BITSTREAM_BIN)
    ddc_add_bitstream_header(${FPGA_TX_BITSTREAM_BIN}
        fpga_bitstream_tx.h ddc_fpga_tx_bitstream DDC_HAS_STORED_TX)
endif()
if(DDC_BITSTREAM_HEADERS)
    add_custom_target(ddc_fpga_bitstream_headers DEPENDS ${DDC_BITSTREAM_HEADERS})
    set(DDC_GENERATED_INCLUDES ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(FPGA_BOOT_MODE STREQUAL "STORED" AND NOT DDC_BITSTREAM_HEADERS)
    message(FATAL_ERROR
        "FPGA_BOOT_MODE=STORED requires FPGA_RX_BITSTREAM_BIN or FPGA_TX_BITSTREAM_BIN")
endif()
if(FPGA_BOOT_MODE STREQUAL "STORED" AND FPGA_DEFAULT_IMAGE STREQUAL "RX"
   AND NOT FPGA_RX_BITSTREAM_BIN)
    message(FATAL_ERROR
        "FPGA_DEFAULT_IMAGE=RX requires FPGA_RX_BITSTREAM_BIN in STORED mode")
endif()
if(FPGA_BOOT_MODE STREQUAL "STORED" AND FPGA_DEFAULT_IMAGE STREQUAL "TX"
   AND NOT FPGA_TX_BITSTREAM_BIN)
    message(FATAL_ERROR
        "FPGA_DEFAULT_IMAGE=TX requires FPGA_TX_BITSTREAM_BIN in STORED mode")
endif()
if(NOT DDC_BITSTREAM_HEADERS)
    message(STATUS
        "FPGA_BITSTREAM_BIN is not set; firmware will wait for a DFU-loaded FPGA")
endif()

set(DDC_SOURCES
    main.c
    usb_descriptors.c
)

if(PICO_BOARD STREQUAL "pico_w")
    list(APPEND DDC_SOURCES openhpsdr.c)
endif()

add_executable(${CMAKE_PROJECT_NAME}
    ${DDC_SOURCES}
)

pico_generate_pio_header(${CMAKE_PROJECT_NAME}
    ${CMAKE_CURRENT_SOURCE_DIR}/i2s_rx.pio
)
pico_generate_pio_header(${CMAKE_PROJECT_NAME}
    ${CMAKE_CURRENT_SOURCE_DIR}/i2s_tx.pio
)
if(DDC_BITSTREAM_HEADERS)
    add_dependencies(${CMAKE_PROJECT_NAME} ddc_fpga_bitstream_headers)
endif()
if(DDC_HAS_STORED_RX)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DDC_HAS_STORED_RX=1)
endif()
if(DDC_HAS_STORED_TX)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DDC_HAS_STORED_TX=1)
endif()
if(FPGA_BOOT_MODE STREQUAL "STORED")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DDC_FPGA_BOOT_FROM_STORED=1)
endif()
if(FPGA_DEFAULT_IMAGE STREQUAL "TX")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DDC_DEFAULT_FPGA_TX=1)
endif()

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    PICO_DEV_ICE=1
)

if(PICO_BOARD STREQUAL "pico_w")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        PICO_CYW43_SUPPORTED=1
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk/include/boards
    ${DDC_GENERATED_INCLUDES}
    ${PICO_TINYUSB_PATH}/hw/bsp/rp2040
    ${PICO_TINYUSB_PATH}/hw/bsp
    ${PICO_TINYUSB_PATH}/hw
    ${PICO_TINYUSB_PATH}/src
    ${PICO_SDK_PATH}/lib/cyw43-driver/src
)

set(DDC_LIBS
    pico_ice_preinit_common
    pico_ice_sdk
    pico_unique_id
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_pio
    pico_multicore
    pico_stdlib
    tinyusb_device
    tinyusb_board
)

if(PICO_BOARD STREQUAL "pico_w")
    list(APPEND DDC_LIBS pico_cyw43_arch_lwip_threadsafe_background)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME}
    ${DDC_LIBS}
)

pico_add_extra_outputs(${CMAKE_PROJECT_NAME})
pico_enable_stdio_usb(${CMAKE_PROJECT_NAME} 0)
pico_enable_stdio_uart(${CMAKE_PROJECT_NAME} 0)
