cmake_minimum_required(VERSION 3.20)
project(firmware C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(FW_PLATFORM "pc" CACHE STRING "Target platform: pc or stm32")
set_property(CACHE FW_PLATFORM PROPERTY STRINGS pc stm32)

option(FW_USE_MBEDTLS "Build TLS/DTLS with mbedTLS"            ON)
option(FW_USE_PAHO    "Build MQTT with Paho Embedded-C"        ON)
option(FW_USE_WAKAAMA "Build LwM2M with Eclipse Wakaama"       ON)

include(cmake/deps.cmake)

# ---- Portable core (everything except platform/) ----------------------------
add_library(fw_core STATIC
    application/device.c
    application/sensor.c
    application/telemetry.c
    protocol/protocol_manager.c
    protocol/mqtt_client.c
    protocol/lwm2m_client.c
    network/network.c
    network/socket.c
    security/tls_common.c
    security/tls.c
    security/dtls.c
    hal/timer.c
    hal/storage.c
    hal/power.c
)
target_include_directories(fw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fw_core PUBLIC
    FW_HAVE_MBEDTLS=$<BOOL:${FW_USE_MBEDTLS}>
    FW_HAVE_PAHO=$<BOOL:${FW_USE_PAHO}>
    FW_HAVE_WAKAAMA=$<BOOL:${FW_USE_WAKAAMA}>
)
if(FW_USE_MBEDTLS)
    target_link_libraries(fw_core PUBLIC mbedtls mbedx509 mbedcrypto)
endif()
if(FW_USE_PAHO)
    target_link_libraries(fw_core PUBLIC paho_mqttpacket)
endif()
if(FW_USE_WAKAAMA)
    target_link_libraries(fw_core PUBLIC wakaama_client)
endif()

if(MSVC)
    target_compile_options(fw_core PRIVATE /W4)
    target_compile_definitions(fw_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(fw_core PRIVATE -Wall -Wextra -Wshadow)
endif()

# ---- Platform ---------------------------------------------------------------
if(FW_PLATFORM STREQUAL "pc")
    add_executable(firmware platform/pc/main.c platform/pc/port_pc.c)
    target_link_libraries(firmware PRIVATE fw_core)
    if(WIN32)
        target_link_libraries(firmware PRIVATE ws2_32 bcrypt)
    else()
        target_link_libraries(firmware PRIVATE m)
    endif()

elseif(FW_PLATFORM STREQUAL "stm32")
    # Built as a library and linked into your STM32CubeMX project, which owns
    # main(), the startup code, the linker script and the toolchain file.
    # From the Cube project's CMakeLists.txt:
    #   set(FW_PLATFORM stm32)
    #   set(FW_STM32_INCLUDE_DIRS <Core/Inc, HAL, CMSIS, LwIP and FreeRTOS include dirs>)
    #   set(FW_STM32_DEFINITIONS  USE_HAL_DRIVER STM32F429xx)
    #   add_subdirectory(firmware)
    #   target_link_libraries(${CMAKE_PROJECT_NAME} firmware_stm32)
    add_library(firmware_stm32 STATIC platform/stm32/port_stm32.c)
    target_include_directories(firmware_stm32 PRIVATE ${FW_STM32_INCLUDE_DIRS})
    target_compile_definitions(firmware_stm32 PRIVATE ${FW_STM32_DEFINITIONS})
    target_link_libraries(firmware_stm32 PUBLIC fw_core)

else()
    message(FATAL_ERROR "Unknown FW_PLATFORM '${FW_PLATFORM}' (expected pc or stm32)")
endif()
