cmake_minimum_required(VERSION 3.13)

include(${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk/cmake/preinit_pico_ice_sdk.cmake)
set(PICO_SDK_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk/lib/pico-sdk)
if(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{PICO_TINYUSB_PATH})
    set(PICO_TINYUSB_PATH "$ENV{PICO_TINYUSB_PATH}" CACHE PATH
        "Path to the TinyUSB checkout used by the DDC firmware")
elseif(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{HOME}
       AND EXISTS "$ENV{HOME}/tinyusb/src/tusb.h"
       AND EXISTS "$ENV{HOME}/tinyusb/hw/bsp/rp2040")
    set(PICO_TINYUSB_PATH "$ENV{HOME}/tinyusb" CACHE PATH
        "Path to the TinyUSB checkout used by the DDC firmware")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ddc_sdr C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pico-ice-sdk pico-ice-sdk)

set(FPGA_BITSTREAM_BIN "" CACHE FILEPATH
    "Packed iCE40 bitstream (.bin) to embed in the RP2040 firmware")

set(DDC_GENERATED_INCLUDES)
if(FPGA_BITSTREAM_BIN)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(FPGA_BITSTREAM_HEADER ${CMAKE_CURRENT_BINARY_DIR}/fpga_bitstream.h)
    add_custom_command(
        OUTPUT ${FPGA_BITSTREAM_HEADER}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2header.py
                ${FPGA_BITSTREAM_BIN}
                ${FPGA_BITSTREAM_HEADER}
        DEPENDS ${FPGA_BITSTREAM_BIN}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2header.py
        VERBATIM
        )
    add_custom_target(ddc_fpga_bitstream_header DEPENDS ${FPGA_BITSTREAM_HEADER})
    set(DDC_GENERATED_INCLUDES ${CMAKE_CURRENT_BINARY_DIR})
else()
    message(STATUS
        "FPGA_BITSTREAM_BIN is not set; firmware will wait for a DFU-loaded FPGA")
endif()

add_executable(${CMAKE_PROJECT_NAME}
    main.c
    usb_descriptors.c
    )
pico_generate_pio_header(${CMAKE_PROJECT_NAME}
    ${CMAKE_CURRENT_SOURCE_DIR}/i2s_rx.pio
    )
if(FPGA_BITSTREAM_BIN)
    add_dependencies(${CMAKE_PROJECT_NAME} ddc_fpga_bitstream_header)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DDC_HAS_EMBEDDED_FPGA=1)
endif()
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${DDC_GENERATED_INCLUDES}
    ${PICO_TINYUSB_PATH}/hw
    ${PICO_TINYUSB_PATH}/src
    )
target_link_libraries(${CMAKE_PROJECT_NAME}
    pico_ice_preinit_common
    pico_ice_sdk
    pico_ice_usb
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_pio
    pico_multicore
    pico_stdlib
    tinyusb_device
    tinyusb_board
    )
pico_add_extra_outputs(${CMAKE_PROJECT_NAME})
pico_enable_stdio_usb(${CMAKE_PROJECT_NAME} 0)
pico_enable_stdio_uart(${CMAKE_PROJECT_NAME} 0)
