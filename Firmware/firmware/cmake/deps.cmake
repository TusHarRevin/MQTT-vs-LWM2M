# Third-party dependencies, fetched at configure time.
#
# To build offline or against a local checkout, point CMake at it instead:
#   -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/path/to/mbedtls
#   -DFETCHCONTENT_SOURCE_DIR_PAHO_EMBEDDED=/path/to/paho.mqtt.embedded-c
#   -DFETCHCONTENT_SOURCE_DIR_WAKAAMA=/path/to/wakaama

include(FetchContent)

# ---- mbedTLS ----------------------------------------------------------------
if(FW_USE_MBEDTLS)
    set(ENABLE_PROGRAMS         OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING          OFF CACHE BOOL "" FORCE)
    set(GEN_FILES               OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS  OFF CACHE BOOL "" FORCE)
    if(FW_PLATFORM STREQUAL "stm32")
        set(MBEDTLS_USER_CONFIG_FILE
            "${CMAKE_CURRENT_LIST_DIR}/../platform/stm32/mbedtls_user_config.h"
            CACHE FILEPATH "" FORCE)
    endif()

    FetchContent_Declare(mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG        v3.6.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(mbedtls)
endif()

# ---- Paho MQTT Embedded-C (MQTTPacket serialiser only) ----------------------
if(FW_USE_PAHO)
    FetchContent_Declare(paho_embedded
        GIT_REPOSITORY https://github.com/eclipse/paho.mqtt.embedded-c.git
        GIT_TAG        v1.1.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake   # download only; we build the sources below
    )
    FetchContent_MakeAvailable(paho_embedded)

    file(GLOB PAHO_SRC ${paho_embedded_SOURCE_DIR}/MQTTPacket/src/*.c)
    add_library(paho_mqttpacket STATIC ${PAHO_SRC})
    target_include_directories(paho_mqttpacket PUBLIC ${paho_embedded_SOURCE_DIR}/MQTTPacket/src)
endif()

# ---- Eclipse Wakaama (LwM2M client) -----------------------------------------
if(FW_USE_WAKAAMA)
    # TODO: pin GIT_TAG to a known-good commit once you've verified a build.
    # Wakaama's source layout has changed over time; the globs and defines
    # below cover both the older (core/er-coap-13) and newer (coap/, data/)
    # layouts, but check them against the revision you pin.
    FetchContent_Declare(wakaama
        GIT_REPOSITORY https://github.com/eclipse-wakaama/wakaama.git
        SOURCE_SUBDIR  _no_cmake
    )
    FetchContent_MakeAvailable(wakaama)

    file(GLOB_RECURSE WAKAAMA_SRC
        ${wakaama_SOURCE_DIR}/core/*.c
        ${wakaama_SOURCE_DIR}/coap/*.c
        ${wakaama_SOURCE_DIR}/data/*.c
    )
    add_library(wakaama_client STATIC ${WAKAAMA_SRC})
    foreach(dir include core coap data)
        if(EXISTS ${wakaama_SOURCE_DIR}/${dir})
            target_include_directories(wakaama_client PUBLIC ${wakaama_SOURCE_DIR}/${dir})
        endif()
    endforeach()
    # PUBLIC: these change struct layouts in liblwm2m.h, so firmware code
    # including it must see the same values.
    target_compile_definitions(wakaama_client PUBLIC
        LWM2M_CLIENT_MODE
        LWM2M_LITTLE_ENDIAN
        LWM2M_COAP_DEFAULT_BLOCK_SIZE=1024
        LWM2M_SUPPORT_TLV
        LWM2M_SUPPORT_SENML_JSON
    )
endif()
