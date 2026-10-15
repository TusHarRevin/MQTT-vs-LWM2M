# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-src")
  file(MAKE_DIRECTORY "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-src")
endif()
file(MAKE_DIRECTORY
  "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-build"
  "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix"
  "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix/tmp"
  "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix/src/wakaama-populate-stamp"
  "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix/src"
  "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix/src/wakaama-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix/src/wakaama-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/_deps/wakaama-subbuild/wakaama-populate-prefix/src/wakaama-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
