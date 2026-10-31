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
    src/doa4_config.cpp
    src/doa4_menu.cpp
    src/doa4_unlocks.cpp
    src/doa4_tweaks.cpp
    src/doa4_backup.cpp
    src/doa4_extras.cpp
    src/doa4_cvars.cpp
    src/doa4_graphics.cpp
    src/doa4_lowmem.cpp
    src/doa4_speakers.cpp
    src/doa4_photo.cpp
    src/doa4_photo_win.cpp
    src/doa4_photo_service.cpp
    src/doa4_photo_notice.cpp
    src/doa4_photo_watch.cpp
    src/doa4_photo_hook.cpp
    src/doa4_xdvdfs.cpp
    src/doa4_gamefiles.cpp
    src/doa4_setup_flow.cpp
    src/doa4_setup_win.cpp
    src/doa4_setup_dialog.cpp
    src/net/np_core.cpp
    src/net/np_exec.cpp
    src/net/np_gekko.cpp
    src/net/np_session.cpp
    src/net/np_det.cpp
    src/net/np_det_middleware.cpp
    src/net/np_det_workers.cpp
    src/net/np_det_trace.cpp
    src/net/np_det_online.cpp
    src/net/np_wire.cpp
    src/net/np_rules.cpp
    src/net/np_handoff.cpp
    src/net/np_transport.cpp
    src/net/np_upnp.cpp
    src/net/np_files.cpp
    src/net/np_online.cpp
    src/net/np_live.cpp
    src/net/np_restart.cpp
)

if(WIN32)
    # The window, taskbar, Explorer and Task Manager icon (src/dead_or_alive_4.rc).
    enable_language(RC)
    add_executable(dead_or_alive_4 WIN32 ${DEADORALIVE4_SOURCES} src/dead_or_alive_4.rc)
else()
    add_executable(dead_or_alive_4 ${DEADORALIVE4_SOURCES})
endif()

rexglue_setup_target(dead_or_alive_4)

# Netplay release identity (plan 1b's CI sets these; local builds are "dev").
set(DOA4_RELEASE_TAG "dev" CACHE STRING "Netplay release tag: netplay-v<major>.<minor>.<run> in CI")
set(DOA4_RELEASE_REPO "Dwreckgg/doa4-recomp" CACHE STRING "GitHub repository of the netplay releases")
set(DOA4_RELEASE_CHANNEL "netplay" CACHE STRING "Netplay release channel (the tag prefix)")
set(DOA4_NETPLAY_VERSION "1.0" CACHE STRING "major.minor of the netplay releases")
target_compile_definitions(dead_or_alive_4 PRIVATE DOA4_RELEASE_TAG="${DOA4_RELEASE_TAG}"
    DOA4_RELEASE_REPO="${DOA4_RELEASE_REPO}" DOA4_RELEASE_CHANNEL="${DOA4_RELEASE_CHANNEL}")

# Netplay: GekkoNet (BSD-2) for the lockstep input exchange, without its ASIO adapter (the port's transport
# carries its packets), and miniupnpc (BSD-3) to open the host's port on the router. Both from the DOA2U
# netplay tree (third_party/README.md).
enable_language(C)
file(GLOB DOA4_GEKKO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GekkoNet/src/*.cpp)
add_library(gekkonet STATIC ${DOA4_GEKKO_SOURCES})
target_include_directories(gekkonet
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GekkoNet/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GekkoNet/include/private
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GekkoNet/thirdparty)
target_compile_definitions(gekkonet PUBLIC GEKKONET_STATIC GEKKONET_NO_ASIO)
set_target_properties(gekkonet PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
set(DOA4_UPNP ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniupnpc)
add_library(miniupnpc STATIC
    ${DOA4_UPNP}/src/igd_desc_parse.c ${DOA4_UPNP}/src/miniupnpc.c ${DOA4_UPNP}/src/minixml.c
    ${DOA4_UPNP}/src/minisoap.c ${DOA4_UPNP}/src/minissdpc.c ${DOA4_UPNP}/src/miniwget.c
    ${DOA4_UPNP}/src/upnpcommands.c ${DOA4_UPNP}/src/upnpdev.c ${DOA4_UPNP}/src/upnpreplyparse.c
    ${DOA4_UPNP}/src/upnperrors.c ${DOA4_UPNP}/src/connecthostport.c ${DOA4_UPNP}/src/portlistingparse.c
    ${DOA4_UPNP}/src/receivedata.c ${DOA4_UPNP}/src/addr_is_reserved.c)
target_include_directories(miniupnpc PUBLIC ${DOA4_UPNP}/include PRIVATE ${DOA4_UPNP} ${DOA4_UPNP}/src)
target_compile_definitions(miniupnpc PUBLIC MINIUPNP_STATICLIB PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(miniupnpc PUBLIC ws2_32 iphlpapi)
target_link_libraries(dead_or_alive_4 PRIVATE gekkonet miniupnpc ws2_32 iphlpapi winhttp bcrypt)

# Local build fix: the ReXGlue SDK at v0.8.0 does not pass ImGui's headers on to the
# game target (src/doa4_diag_overlay.cpp includes <imgui.h>); ImGui itself is
# already compiled into the SDK's UI library.
if(TARGET imgui)
    target_include_directories(dead_or_alive_4 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# Local build fix: with REXSDK_DIR (the SDK built from source alongside the game), the
# SDK's generated headers (rex/version.h) sit in its build folder, which the game
# target's own SDK sources (ui/rex_app.cpp) do not search.
if(REXSDK_DIR)
    target_include_directories(dead_or_alive_4 PRIVATE ${CMAKE_BINARY_DIR}/rexglue-sdk/include)
endif()
# Netplay hashing (src/net/np_session.cpp) uses rex/hash.h, whose xxhash.h the SDK
# keeps to its own targets; header-only use (XXH_INLINE_ALL), nothing to link.
if(TARGET xxhash)
    target_include_directories(dead_or_alive_4 PRIVATE $<TARGET_PROPERTY:xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# Netplay (spec section 4, FMA row): no floating-point contraction anywhere in the game target, so no
# compiler or flag change can fuse a*b+c differently on one player's build. Phase 0 measured no
# contraction without it (phase0_facts.md 3); this keeps it that way.
target_compile_options(dead_or_alive_4 PRIVATE -ffp-contract=off)
