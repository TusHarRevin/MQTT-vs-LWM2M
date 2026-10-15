# Cross-compiling for Cortex-M4F with the GNU Arm toolchain.
# Default: the one in STM32CubeCLT. Override with -DARM_TOOLCHAIN_DIR=<dir with arm-none-eabi-gcc>.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ARM_TOOLCHAIN_DIR)
    if(DEFINED ENV{ARM_TOOLCHAIN_DIR})
        set(ARM_TOOLCHAIN_DIR $ENV{ARM_TOOLCHAIN_DIR})
    elseif(EXISTS "R:/STM_driver_VS/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin")
        set(ARM_TOOLCHAIN_DIR "R:/STM_driver_VS/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin")
    endif()
endif()
set(ARM_TOOLCHAIN_DIR "${ARM_TOOLCHAIN_DIR}" CACHE PATH "directory containing arm-none-eabi-gcc")

if(WIN32 OR CMAKE_HOST_WIN32)
    set(_exe ".exe")
endif()
if(ARM_TOOLCHAIN_DIR)
    set(_p "${ARM_TOOLCHAIN_DIR}/")
endif()
set(CMAKE_C_COMPILER   "${_p}arm-none-eabi-gcc${_exe}")
set(CMAKE_ASM_COMPILER "${_p}arm-none-eabi-gcc${_exe}")
set(CMAKE_OBJCOPY      "${_p}arm-none-eabi-objcopy${_exe}" CACHE FILEPATH "")
set(CMAKE_SIZE         "${_p}arm-none-eabi-size${_exe}" CACHE FILEPATH "")

# Compiler checks can't link a bare-metal executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} --specs=nosys.specs -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
