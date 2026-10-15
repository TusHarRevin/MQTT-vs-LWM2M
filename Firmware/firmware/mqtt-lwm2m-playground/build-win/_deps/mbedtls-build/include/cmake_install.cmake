# Install script for directory: R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/mqtt_lwm2m_playground")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/Users/SAT CARD/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/objdump.exe")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/aes.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/aria.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/asn1.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/asn1write.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/base64.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/bignum.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/block_cipher.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/build_info.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/camellia.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ccm.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/chacha20.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/chachapoly.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/check_config.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/cipher.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/cmac.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/compat-2.x.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_adjust_legacy_crypto.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_adjust_legacy_from_psa.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_adjust_psa_from_legacy.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_adjust_psa_superset_legacy.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_adjust_ssl.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_adjust_x509.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/config_psa.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/constant_time.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ctr_drbg.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/debug.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/des.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/dhm.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ecdh.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ecdsa.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ecjpake.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ecp.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/entropy.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/error.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/gcm.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/hkdf.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/hmac_drbg.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/lms.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/mbedtls_config.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/md.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/md5.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/memory_buffer_alloc.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/net_sockets.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/nist_kw.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/oid.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/pem.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/pk.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/pkcs12.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/pkcs5.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/pkcs7.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/platform.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/platform_time.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/platform_util.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/poly1305.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/private_access.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/psa_util.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ripemd160.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/rsa.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/sha1.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/sha256.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/sha3.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/sha512.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ssl.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ssl_cache.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ssl_ciphersuites.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ssl_cookie.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/ssl_ticket.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/threading.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/timing.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/version.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/x509.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/x509_crl.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/x509_crt.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/mbedtls/x509_csr.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/psa" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/build_info.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_adjust_auto_enabled.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_adjust_config_dependencies.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_adjust_config_key_pair_types.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_adjust_config_synonyms.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_builtin_composites.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_builtin_key_derivation.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_builtin_primitives.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_compat.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_config.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_driver_common.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_driver_contexts_composites.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_driver_contexts_key_derivation.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_driver_contexts_primitives.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_extra.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_legacy.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_platform.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_se_driver.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_sizes.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_struct.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_types.h"
    "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-src/include/psa/crypto_values.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/mbedtls-build/include/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
