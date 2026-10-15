# CMake generated Testfile for 
# Source directory: R:/Revin/Firmware/firmware/mqtt-lwm2m-playground
# Build directory: R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[sensor]=] "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/tests/test_sensor.exe")
set_tests_properties([=[sensor]=] PROPERTIES  _BACKTRACE_TRIPLES "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;103;add_test;R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;0;")
add_test([=[bench]=] "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/tests/test_bench.exe")
set_tests_properties([=[bench]=] PROPERTIES  _BACKTRACE_TRIPLES "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;103;add_test;R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;0;")
add_test([=[telemetry]=] "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/tests/test_telemetry.exe")
set_tests_properties([=[telemetry]=] PROPERTIES  _BACKTRACE_TRIPLES "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;103;add_test;R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;0;")
add_test([=[netsim]=] "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/tests/test_netsim.exe")
set_tests_properties([=[netsim]=] PROPERTIES  _BACKTRACE_TRIPLES "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;103;add_test;R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;0;")
add_test([=[coap_trace]=] "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/build-win/tests/test_coap_trace.exe")
set_tests_properties([=[coap_trace]=] PROPERTIES  _BACKTRACE_TRIPLES "R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;103;add_test;R:/Revin/Firmware/firmware/mqtt-lwm2m-playground/CMakeLists.txt;0;")
subdirs("_deps/mbedtls-build")
