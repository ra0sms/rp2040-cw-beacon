# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD waveshare_rp2040_zero CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(Beacon C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(Beacon Beacon.c )

set(BEACON_VERSION "0.2")
pico_set_program_name(Beacon "Beacon")
pico_set_program_version(Beacon "${BEACON_VERSION}")

# version.h (program version + git describe), regenerated on every build
add_custom_target(beacon_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_LIST_DIR}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/version.h
            -DVERSION=${BEACON_VERSION}
            -P ${CMAKE_CURRENT_LIST_DIR}/version.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/version.h)
add_dependencies(Beacon beacon_version)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(Beacon 0)
pico_enable_stdio_usb(Beacon 1)

# Add the standard library to the build
target_link_libraries(Beacon
        pico_stdlib
        pico_status_led
        hardware_pwm
        hardware_flash
        hardware_watchdog)

# Add the standard include files to the build
target_include_directories(Beacon PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

pico_add_extra_outputs(Beacon)

