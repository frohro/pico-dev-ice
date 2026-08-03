cmake_minimum_required(VERSION 3.13)

# Research: 48/96 kHz PCM1808 UAC1 SDR for v0.2 board
#
# Supports:
#   • 48/96 kHz for PCM1808 (24-bit S24_3LE USB stream)
#   • Si5351a CLK0 = 24.576 MHz → PCM1808 SCKI master clock

include(pico_sdk_import.cmake)

if(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{HOME})
    set(PICO_TINYUSB_PATH "$ENV{HOME}/tinyusb" CACHE PATH "Path to TinyUSB")
endif()

project(sdr_2026_v0_2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdr_2026_v0_2
    main.c
    si5351.c
    usb_descriptors.c
)

pico_generate_pio_header(sdr_2026_v0_2 ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

target_include_directories(sdr_2026_v0_2 PRIVATE 
    .
    ${PICO_TINYUSB_PATH}/hw/bsp
    ${PICO_TINYUSB_PATH}/src
)

target_link_libraries(sdr_2026_v0_2
    pico_stdlib
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_clocks
    hardware_vreg
    pico_multicore
    tinyusb_device
    tinyusb_board
)

pico_enable_stdio_usb(sdr_2026_v0_2 0)
pico_enable_stdio_uart(sdr_2026_v0_2 1)

pico_add_extra_outputs(sdr_2026_v0_2)
