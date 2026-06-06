# dead_or_alive_4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(dead_or_alive_4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(DEADORALIVE4_SOURCES
    src/main.cpp
    src/doa4_app_hooks.cpp
    src/doa4_debug_hooks.cpp
    src/doa4_diag_overlay.cpp
)

if(WIN32)
    add_executable(dead_or_alive_4 WIN32 ${DEADORALIVE4_SOURCES})
else()
    add_executable(dead_or_alive_4 ${DEADORALIVE4_SOURCES})
endif()

rexglue_setup_target(dead_or_alive_4)
