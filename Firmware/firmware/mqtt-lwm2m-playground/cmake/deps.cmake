# Third-party libraries, fetched at configure time (needs git + network the
# first time; afterwards they live in build/_deps).
#
# Offline / local checkout:
#   -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/path/to/mbedtls
#   -DFETCHCONTENT_SOURCE_DIR_PAHO=/path/to/paho.mqtt.embedded-c
#   -DFETCHCONTENT_SOURCE_DIR_WAKAAMA=/path/to/wakaama

include(FetchContent)

# ---- mbedTLS (TLS for MQTT, DTLS for LwM2M, SHA-256 for OTA) ---------------
set(ENABLE_PROGRAMS        OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING         OFF CACHE BOOL "" FORCE)
set(GEN_FILES              OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v3.6.7
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(mbedtls)

# ---- Paho MQTT Embedded-C: only the MQTTPacket serialiser -------------------
# MQTTPacket is pure packet encode/decode with no I/O, which is exactly what we
# want: the playground owns the socket so it can time and count every byte.
FetchContent_Declare(paho
    GIT_REPOSITORY https://github.com/eclipse/paho.mqtt.embedded-c.git
    GIT_TAG        v1.1.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake
)
FetchContent_MakeAvailable(paho)
file(GLOB PAHO_SRC ${paho_SOURCE_DIR}/MQTTPacket/src/*.c)
add_library(paho_mqttpacket STATIC ${PAHO_SRC})
target_include_directories(paho_mqttpacket PUBLIC ${paho_SOURCE_DIR}/MQTTPacket/src)
if(NOT MSVC)
    target_compile_options(paho_mqttpacket PRIVATE -w)
endif()

# ---- Eclipse Wakaama (LwM2M client core) ------------------------------------
# Pinned to the 2026-05-26 snapshot. We compile the sources ourselves instead
# of including wakaama.cmake, which also pulls in tinydtls and the examples.
FetchContent_Declare(wakaama
    GIT_REPOSITORY https://github.com/eclipse-wakaama/wakaama.git
    GIT_TAG        94ff56f77a2d24a5890e0e703809a47633aa7d4b
    SOURCE_SUBDIR  _no_cmake
)
FetchContent_MakeAvailable(wakaama)

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
# PUBLIC: these change struct layouts in liblwm2m.h, so our code must see them too.
target_compile_definitions(wakaama_client PUBLIC
    LWM2M_CLIENT_MODE
    LWM2M_LITTLE_ENDIAN
    LWM2M_SUPPORT_TLV
    LWM2M_SUPPORT_JSON
    LWM2M_SUPPORT_SENML_JSON
    LWM2M_SUPPORT_SENML_CBOR
    LWM2M_OLD_CONTENT_FORMAT_SUPPORT
    LWM2M_COAP_DEFAULT_BLOCK_SIZE=1024
    LWM2M_COAP_MAX_MESSAGE_SIZE=2048
    LWM2M_COAP_DEFAULT_MAX_RETRANSMIT=4
    LWM2M_COAP_SEPARATE_TIMEOUT=15
    LWM2M_LOG_LEVEL=LWM2M_LOG_DISABLED
    LWM2M_LOG_MAX_MSG_TXT_SIZE=200
)
if(NOT MSVC)
    target_compile_options(wakaama_client PRIVATE -w)
endif()
