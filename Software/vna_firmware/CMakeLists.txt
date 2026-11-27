cmake_minimum_required(VERSION 3.13)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
elseif(DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    include("$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
elseif(EXISTS "$ENV{HOME}/.pico-sdk/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk")
    include("$ENV{HOME}/.pico-sdk/external/pico_sdk_import.cmake")
else()
    message(FATAL_ERROR
        "Cannot find pico_sdk_import.cmake. Copy it from $PICO_SDK_PATH/external/pico_sdk_import.cmake "
        "or export PICO_SDK_PATH before running cmake.")
endif()

project(pico_vna C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_vna
    main.c
    fpga_vna.c
    nanovna_shell.c
)

target_include_directories(pico_vna PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(pico_vna PRIVATE -Wall -Wextra)

target_link_libraries(pico_vna
    pico_stdlib
    hardware_spi
)

pico_enable_stdio_usb(pico_vna 1)
pico_enable_stdio_uart(pico_vna 0)

pico_add_extra_outputs(pico_vna)
