# Firmware for NUCLEO-L452RE-P + Quectel EC200U: the playground's MQTT and
# LwM2M devices running on real hardware over LTE.
#
#   cmake -S hardware/stm32l452-ec200u -B build-stm32 -G Ninja
#         -DCMAKE_TOOLCHAIN_FILE=hardware/stm32l452-ec200u/cmake/arm-none-eabi.cmake
#   cmake --build build-stm32
#   cmake --build build-stm32 --target flash      (ST-LINK, STM32CubeProgrammer CLI)
#
# See README.md in this folder.

cmake_minimum_required(VERSION 3.20)
project(pg_stm32l452_ec200u C ASM)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

get_filename_component(PG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HW ${CMAKE_CURRENT_SOURCE_DIR})

# ---- STM32CubeL4 (HAL + CMSIS) ---------------------------------------------------
set(STM32CUBE_L4 "$ENV{USERPROFILE}/STM32Cube/Repository/STM32Cube_FW_L4_V1.18.2" CACHE PATH
    "STM32CubeL4 firmware package (installed by STM32CubeIDE/CubeMX)")
if(NOT EXISTS "${STM32CUBE_L4}/Drivers/STM32L4xx_HAL_Driver")
    message(FATAL_ERROR "STM32CubeL4 not found at ${STM32CUBE_L4} - pass -DSTM32CUBE_L4=<path>")
endif()
set(HAL ${STM32CUBE_L4}/Drivers/STM32L4xx_HAL_Driver)
add_library(stm32_hal STATIC
    ${HW}/cube/system_stm32l4xx.c
    ${HAL}/Src/stm32l4xx_hal.c
    ${HAL}/Src/stm32l4xx_hal_cortex.c
    ${HAL}/Src/stm32l4xx_hal_dma.c
    ${HAL}/Src/stm32l4xx_hal_flash.c
    ${HAL}/Src/stm32l4xx_hal_flash_ex.c
    ${HAL}/Src/stm32l4xx_hal_gpio.c
    ${HAL}/Src/stm32l4xx_hal_pwr.c
    ${HAL}/Src/stm32l4xx_hal_pwr_ex.c
    ${HAL}/Src/stm32l4xx_hal_rcc.c
    ${HAL}/Src/stm32l4xx_hal_rcc_ex.c
    ${HAL}/Src/stm32l4xx_hal_rng.c
    ${HAL}/Src/stm32l4xx_hal_tim.c
    ${HAL}/Src/stm32l4xx_hal_tim_ex.c
    ${HAL}/Src/stm32l4xx_hal_uart.c
    ${HAL}/Src/stm32l4xx_hal_uart_ex.c
)
target_include_directories(stm32_hal PUBLIC
    ${HW}/cube ${HW}/src
    ${HAL}/Inc
    ${STM32CUBE_L4}/Drivers/CMSIS/Device/ST/STM32L4xx/Include
    ${STM32CUBE_L4}/Drivers/CMSIS/Include)
target_compile_definitions(stm32_hal PUBLIC STM32L452xx USE_HAL_DRIVER)
target_compile_options(stm32_hal PRIVATE -w)

# ---- Third-party libraries ----------------------------------------------------------
# Same versions as the PC build. Reuses the PC build's copies when present
# (build/_src or build/_deps), otherwise fetches them with git.
include(FetchContent)
foreach(dep mbedtls paho wakaama)
    string(TOUPPER ${dep} DEP)
    foreach(cand "${PG_ROOT}/build/_src/${dep}" "${PG_ROOT}/build/_deps/${dep}-src")
        if(NOT FETCHCONTENT_SOURCE_DIR_${DEP} AND EXISTS "${cand}/README.md")
            set(FETCHCONTENT_SOURCE_DIR_${DEP} "${cand}")
        endif()
    endforeach()
endforeach()
FetchContent_Declare(mbedtls GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git GIT_TAG v3.6.7
                     GIT_SHALLOW TRUE SOURCE_SUBDIR _no_cmake)
FetchContent_Declare(paho GIT_REPOSITORY https://github.com/eclipse/paho.mqtt.embedded-c.git GIT_TAG v1.1.0
                     GIT_SHALLOW TRUE SOURCE_SUBDIR _no_cmake)
FetchContent_Declare(wakaama GIT_REPOSITORY https://github.com/eclipse-wakaama/wakaama.git
                     GIT_TAG 94ff56f77a2d24a5890e0e703809a47633aa7d4b SOURCE_SUBDIR _no_cmake)
FetchContent_MakeAvailable(mbedtls paho wakaama)

file(GLOB MBEDTLS_SRC ${mbedtls_SOURCE_DIR}/library/*.c)
add_library(mbedtls_stm32 STATIC ${MBEDTLS_SRC})
target_include_directories(mbedtls_stm32 PUBLIC ${mbedtls_SOURCE_DIR}/include ${HW}/src
                                         PRIVATE ${mbedtls_SOURCE_DIR}/library)
target_compile_definitions(mbedtls_stm32 PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config_stm32.h")
target_compile_options(mbedtls_stm32 PRIVATE -w)

file(GLOB PAHO_SRC ${paho_SOURCE_DIR}/MQTTPacket/src/*.c)
add_library(paho_mqttpacket STATIC ${PAHO_SRC})
target_include_directories(paho_mqttpacket PUBLIC ${paho_SOURCE_DIR}/MQTTPacket/src)
target_compile_options(paho_mqttpacket PRIVATE -w)

set(W ${wakaama_SOURCE_DIR})
add_library(wakaama_client STATIC
    ${W}/core/bootstrap.c ${W}/core/discover.c ${W}/core/liblwm2m.c
    ${W}/core/list.c ${W}/core/logging.c ${W}/core/management.c
    ${W}/core/objects.c ${W}/core/observe.c ${W}/core/packet.c
    ${W}/core/registration.c ${W}/core/uri.c ${W}/core/utils.c
    ${W}/core/reporting.c
    ${W}/coap/block.c ${W}/coap/transaction.c ${W}/coap/er-coap-13/er-coap-13.c
    ${W}/data/data.c ${W}/data/json.c ${W}/data/json_common.c
    ${W}/data/senml_json.c ${W}/data/tlv.c ${W}/data/cbor.c
    ${W}/data/senml_common.c ${W}/data/senml_cbor.c
)
target_include_directories(wakaama_client PUBLIC ${W}/include PRIVATE ${W}/core ${W}/coap)
target_compile_definitions(wakaama_client PUBLIC   # same as the PC build (cmake/deps.cmake)
    LWM2M_CLIENT_MODE LWM2M_LITTLE_ENDIAN LWM2M_SUPPORT_TLV LWM2M_SUPPORT_JSON
    LWM2M_SUPPORT_SENML_JSON LWM2M_SUPPORT_SENML_CBOR LWM2M_OLD_CONTENT_FORMAT_SUPPORT
    LWM2M_COAP_DEFAULT_BLOCK_SIZE=1024 LWM2M_COAP_MAX_MESSAGE_SIZE=2048
    LWM2M_COAP_DEFAULT_MAX_RETRANSMIT=4 LWM2M_COAP_SEPARATE_TIMEOUT=15
    LWM2M_LOG_LEVEL=LWM2M_LOG_DISABLED LWM2M_LOG_MAX_MSG_TXT_SIZE=200)
target_compile_options(wakaama_client PRIVATE -w)

# ---- Embedded files (read with fopen by link.c) ---------------------------------------
set(ROMFS_FILES ${PG_ROOT}/servers/mosquitto/certs/ca.crt        # lab CA (local Mosquitto)
                ${PG_ROOT}/servers/public/public-mqtt-ca.pem)    # broker.emqx.io + test.mosquitto.org (lab --public)
set(ROMFS_C ${CMAKE_BINARY_DIR}/romfs_data.c)
set(romfs "/* generated by CMakeLists.txt - do not edit */\n#include \"romfs.h\"\n")
set(romfs_table "")
set(i 0)
foreach(f ${ROMFS_FILES})
    file(READ ${f} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    get_filename_component(name ${f} NAME)
    file(SIZE ${f} size)
    string(APPEND romfs "static const unsigned char f${i}[] = {${hex}};\n")
    string(APPEND romfs_table "    { \"${name}\", f${i}, ${size} },\n")
    math(EXPR i "${i} + 1")
endforeach()
string(APPEND romfs "const romfs_file_t romfs_files[] = {\n${romfs_table}    { 0, 0, 0 },\n};\n")
file(CONFIGURE OUTPUT ${ROMFS_C} CONTENT "${romfs}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ROMFS_FILES})

# ---- Firmware -------------------------------------------------------------------------------
set(D ${PG_ROOT}/device)
add_executable(pg_firmware
    ${HW}/cube/startup_stm32l452retx.s
    ${HW}/src/main.c ${HW}/src/board.c ${HW}/src/ec200u.c ${HW}/src/pal_ec200u.c
    ${HW}/src/syscalls.c ${ROMFS_C}
    # the playground's device code, unchanged
    ${D}/common/sensor.c ${D}/common/telemetry.c ${D}/common/netsim.c ${D}/common/link.c
    ${D}/common/events.c ${D}/common/control.c ${D}/common/cli.c ${PG_ROOT}/benchmark/bench.c
    ${D}/mqtt/main.c ${D}/mqtt/mqtt_client.c
    ${D}/lwm2m/main.c ${D}/lwm2m/lwm2m_client.c ${D}/lwm2m/objects.c ${D}/lwm2m/coap_trace.c
)
set_source_files_properties(${D}/mqtt/main.c PROPERTIES COMPILE_DEFINITIONS main=pg_mqtt_main)
set_source_files_properties(${D}/lwm2m/main.c PROPERTIES COMPILE_DEFINITIONS main=pg_lwm2m_main)
target_include_directories(pg_firmware PRIVATE ${HW}/src ${D}/common ${D}/mqtt ${D}/lwm2m ${PG_ROOT}/benchmark)
set(HW_DEFINES "" CACHE STRING "extra defines for hw_config.h, e.g. MODEM_APN=\"jionet\"")
target_compile_definitions(pg_firmware PRIVATE
    # RAM budget: the PC sizes are far bigger than 160 KiB allows
    MQTT_BUF_SIZE=4096 LINK_QUEUE_LEN=32 LINK_SCRATCH_SIZE=2048 EV_JSON_MAX=4096
    MAX_PENDING=8 PENDING_BUF_SIZE=1152 LWC_RX_BUF_SIZE=2048
    ${HW_DEFINES})
# -Wno-format: the shared code prints uint32_t with %u; on arm-none-eabi that
# is "long unsigned int" (same 32 bits, so it prints correctly) and GCC warns.
target_compile_options(pg_firmware PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
                                           -Wno-missing-field-initializers -Wno-format)
target_link_libraries(pg_firmware PRIVATE stm32_hal mbedtls_stm32 paho_mqttpacket wakaama_client m)
target_link_options(pg_firmware PRIVATE
    -T${HW}/cube/STM32L452RETX_FLASH.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/pg_firmware.map -Wl,--print-memory-usage)
set_target_properties(pg_firmware PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${HW}/cube/STM32L452RETX_FLASH.ld)

add_custom_command(TARGET pg_firmware POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:pg_firmware> ${CMAKE_BINARY_DIR}/pg_firmware.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:pg_firmware> ${CMAKE_BINARY_DIR}/pg_firmware.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:pg_firmware>)

# ---- Flash over ST-LINK --------------------------------------------------------------------
find_program(STM32_PROGRAMMER STM32_Programmer_CLI
    HINTS "R:/STM_driver_VS/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin"
          "C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin")
if(STM32_PROGRAMMER)
    add_custom_target(flash
        COMMAND ${STM32_PROGRAMMER} -c port=SWD -w $<TARGET_FILE:pg_firmware> -v -rst
        DEPENDS pg_firmware USES_TERMINAL)
endif()
