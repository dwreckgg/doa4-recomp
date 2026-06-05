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

# --- TEMPORARY crash-localization diagnostic ---------------------------------
# The Start-screen crash is a guest NULL-pointer write inside sub_82386440
# (generated/default/dead_or_alive_4_recomp.5.cpp). At -O2 the recompiler keeps
# the guest PPC context in host registers, so the [doa4][CRASH] dump shows stale
# r31/lr and only an approximate line. Building just this one TU at -O0 forces
# every ctx.rN update to memory, so the crash dump reports the exact faulting
# instruction and the real null register. Remove once the root cause is found.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dead_or_alive_4_recomp.5.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0")
