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

set(FPGA_BITSTREAM_BIN "${CMAKE_CURRENT_SOURCE_DIR}/bitstream/vna_top.bin" CACHE FILEPATH
    "Packed iCE40 VNA bitstream (.bin) to embed in the RP2040 firmware")

if(NOT EXISTS "${FPGA_BITSTREAM_BIN}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bitstreams/vna_top.bin")
    set(FPGA_BITSTREAM_BIN "${CMAKE_CURRENT_SOURCE_DIR}/bitstreams/vna_top.bin")
endif()

if(NOT EXISTS "${FPGA_BITSTREAM_BIN}")
    message(FATAL_ERROR
        "Cannot find VNA FPGA bitstream at '${FPGA_BITSTREAM_BIN}'. "
        "Place vna_top.bin in bitstream/ or pass -DFPGA_BITSTREAM_BIN=/path/to/vna_top.bin.")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(VNA_BITSTREAM_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vna_bitstream.h")
add_custom_command(
    OUTPUT ${VNA_BITSTREAM_HEADER}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2header.py
            ${FPGA_BITSTREAM_BIN}
            ${VNA_BITSTREAM_HEADER}
            vna_fpga_bitstream
    DEPENDS ${FPGA_BITSTREAM_BIN}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2header.py
    VERBATIM
)
add_custom_target(vna_fpga_bitstream_header DEPENDS ${VNA_BITSTREAM_HEADER})

add_executable(pico_vna
    main.c
    fpga_vna.c
    nanovna_shell.c
)

add_dependencies(pico_vna vna_fpga_bitstream_header)
target_compile_definitions(pico_vna PRIVATE VNA_HAS_STORED_BITSTREAM=1)
target_include_directories(pico_vna PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(pico_vna PRIVATE -Wall -Wextra)

target_link_libraries(pico_vna
    pico_stdlib
    hardware_spi
)

pico_enable_stdio_usb(pico_vna 1)
pico_enable_stdio_uart(pico_vna 0)

pico_add_extra_outputs(pico_vna)
