cmake_minimum_required(VERSION 3.20)
project(mqtt_lwm2m_playground C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Put every executable directly in build/ so `./build/mqtt_device` works with
# single- and multi-config generators alike.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR})
endforeach()

include(cmake/deps.cmake)

if(MSVC)
    set(PG_WARNINGS /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    set(PG_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-missing-field-initializers)
endif()

if(WIN32)
    set(PG_SYS_LIBS ws2_32 bcrypt)
else()
    find_package(Threads REQUIRED)
    set(PG_SYS_LIBS m Threads::Threads)
endif()

# ---- Shared device code ------------------------------------------------------
# Everything protocol-independent: sensor model, transport (socket -> netsim ->
# TLS/DTLS), lab event reporting and benchmark statistics.
add_library(pg_common STATIC
    device/common/pal.c
    device/common/sensor.c
    device/common/telemetry.c
    device/common/netsim.c
    device/common/link.c
    device/common/events.c
    device/common/control.c
    device/common/cli.c
    benchmark/bench.c
)
target_include_directories(pg_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/device/common ${CMAKE_CURRENT_SOURCE_DIR}/benchmark)
target_compile_definitions(pg_common PUBLIC PG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(pg_common PRIVATE ${PG_WARNINGS})
target_link_libraries(pg_common PUBLIC mbedtls mbedx509 mbedcrypto ${PG_SYS_LIBS})

# ---- Milestone 1: MQTT device ------------------------------------------------
add_executable(mqtt_device device/mqtt/main.c device/mqtt/mqtt_client.c)
target_compile_options(mqtt_device PRIVATE ${PG_WARNINGS})
target_link_libraries(mqtt_device PRIVATE pg_common paho_mqttpacket)

# ---- Milestone 2: LwM2M device -----------------------------------------------
add_executable(lwm2m_device device/lwm2m/main.c device/lwm2m/lwm2m_client.c
                            device/lwm2m/objects.c device/lwm2m/coap_trace.c)
target_compile_options(lwm2m_device PRIVATE ${PG_WARNINGS})
target_link_libraries(lwm2m_device PRIVATE pg_common wakaama_client)

# ---- Simulated OTA -----------------------------------------------------------
add_executable(ota_device ota/ota_device.c)
target_compile_options(ota_device PRIVATE ${PG_WARNINGS})
target_link_libraries(ota_device PRIVATE pg_common)

add_executable(make_firmware ota/make_firmware.c)
target_compile_options(make_firmware PRIVATE ${PG_WARNINGS})
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ota/firmware_v1.bin ${CMAKE_BINARY_DIR}/ota/firmware_v2.bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/ota
    COMMAND make_firmware ${CMAKE_BINARY_DIR}/ota/firmware_v1.bin 1.0.0 65536
    COMMAND make_firmware ${CMAKE_BINARY_DIR}/ota/firmware_v2.bin 2.0.0 262144
    DEPENDS make_firmware
    COMMENT "Generating sample firmware images (SIMULATED OTA)"
)
add_custom_target(firmware_images ALL DEPENDS ${CMAKE_BINARY_DIR}/ota/firmware_v1.bin)

# ---- Lab server: dashboard + event hub + Leshan proxy ------------------------
add_executable(lab dashboard/server/lab.c dashboard/server/http.c
                   dashboard/server/proc.c dashboard/server/monitor.c dashboard/server/serial.c
                   dashboard/server/upstream.c)
target_compile_options(lab PRIVATE ${PG_WARNINGS})
target_link_libraries(lab PRIVATE pg_common paho_mqttpacket)

# ---- Tests -------------------------------------------------------------------
enable_testing()
foreach(t sensor bench telemetry netsim coap_trace)
    set(extra)
    if(t STREQUAL "coap_trace")
        set(extra device/lwm2m/coap_trace.c)
    endif()
    add_executable(test_${t} tests/test_${t}.c ${extra})
    target_include_directories(test_${t} PRIVATE tests device/lwm2m)
    target_link_libraries(test_${t} PRIVATE pg_common)
    set_target_properties(test_${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${cfg} CFG)
        set_target_properties(test_${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/tests)
    endforeach()
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
