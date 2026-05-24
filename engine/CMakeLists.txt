project(osengine)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match cscript_core's build type (see dts-viewer/CMakeLists.txt header
# comment about TORQUE_DEBUG ODR).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 3space + its conan deps. Mirror the find_package list dts-viewer uses;
# osengine pulls the same headers but doesn't link SDL/GL/audio.
# CMAKE_CURRENT_LIST_DIR resolves relative to THIS file regardless of
# how cmake was invoked (top-level vs add_subdirectory).
set(THREESPACE_DIR ${CMAKE_CURRENT_LIST_DIR}/../3space)
list(APPEND CMAKE_MODULE_PATH ${THREESPACE_DIR}/build/cmake)

find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED)
find_package(glm REQUIRED)
find_package(span-lite REQUIRED)
find_package(pegtl REQUIRED)
find_package(libzip REQUIRED)

# osengine library. Track 26 spec 01 — everything platform-free here.
add_library(osengine STATIC
    src/ai_bindings.cpp
    src/ai_perception.cpp
    src/ai_player.cpp
    src/ai_tick.cpp
    src/bot_paths.cpp
    src/chat_channel.cpp
    src/client_events.cpp
    src/cscript_host.cpp
    src/damage_resolver.cpp
    src/entity_state.cpp
    src/flag_state.cpp
    src/ghost_emitter.cpp
    src/ghost_encoder.cpp
    src/ghost_stream.cpp
    src/ghost_types.cpp
    src/hud_bindings.cpp
    src/inv_station.cpp
    src/listen_server.cpp
    src/map_cycle.cpp
    src/match_state.cpp
    src/mission.cpp
    src/mission_bounds.cpp
    src/mission_loader.cpp
    src/mission_sounds.cpp
    src/movecommand.cpp
    src/net_client.cpp
    src/null_audio_sink.cpp
    src/paths.cpp
    src/server_listener.cpp
    src/tah_ghost_burst.cpp
    src/session_table.cpp
    src/team_assigner.cpp
    src/world_tick.cpp
    src/physics_capsule.cpp
    src/player_controller.cpp
    src/projectile_world.cpp
    src/player_simobject.cpp
    src/reliable_acks.cpp
    src/scoreboard.cpp
    src/vehicle.cpp
    src/wav_loader.cpp
    src/weapons.cpp)

target_include_directories(osengine PUBLIC
    include
    include/osengine)  # so dts-viewer's existing unqualified #include "X.hpp"
                       # for engine headers keeps working without sweeping
                       # the codebase

target_include_directories(osengine PRIVATE
    ${THREESPACE_DIR}/include
    ${THREESPACE_DIR}/cscript
    ${THREESPACE_DIR}/cscript/console
    ${THREESPACE_DIR}/cscript/console/torquescript
    ${THREESPACE_DIR}/cscript/math/public
    ${THREESPACE_DIR}/cscript/math/impl
    ${THREESPACE_DIR}/cscript/lib
    ${THREESPACE_DIR}/cscript/lib/tinyxml2)

# osengine consumers (dts-viewer, server, client) get the same private
# include paths transitively via target_link_libraries(... osengine).
# Re-expose them as INTERFACE so users don't have to repeat them.
target_include_directories(osengine INTERFACE
    ${THREESPACE_DIR}/include
    ${THREESPACE_DIR}/cscript
    ${THREESPACE_DIR}/cscript/console
    ${THREESPACE_DIR}/cscript/console/torquescript
    ${THREESPACE_DIR}/cscript/math/public
    ${THREESPACE_DIR}/cscript/math/impl
    ${THREESPACE_DIR}/cscript/lib
    ${THREESPACE_DIR}/cscript/lib/tinyxml2)

target_link_libraries(osengine PUBLIC
    nlohmann_json::nlohmann_json
    Boost::Boost
    glm::glm
    nonstd::span-lite
    taocpp::pegtl
    libzip::zip)

# cscript_core (Torque3D heritage) uses SDL2 for its thread/mutex/semaphore
# primitives — see 3space/cscript/.../platformSDL_threads/. osengine itself
# never touches SDL, but cscript_core's mutex/thread/semaphore .obj files
# carry undefined refs to SDL_CreateMutex, SDL_CreateThread, etc.
#
# We do NOT propagate lib3space.a / libcscript_core.a / SDL2 through
# osengine PUBLIC because CMake reorders / dedupes link libraries and on
# Windows MinGW (ld.bfd, single-pass static-archive resolution) the
# ordering must be EXACTLY: lib3space.a -> libcscript_core.a -> SDL2 so
# SDL2 can satisfy cscript_core's refs. Wrapping in --start-group/--end-
# group from PUBLIC didn't work — CMake stripped the grouping flags.
#
# Instead, export the .a paths as cache variables and let each consumer
# (dts-viewer, server, client) list them explicitly in their own
# target_link_libraries call.
set(OSENGINE_LIB3SPACE_A     "${THREESPACE_DIR}/build/lib3space.a"
    CACHE INTERNAL "lib3space static archive")
set(OSENGINE_CSCRIPT_CORE_A  "${THREESPACE_DIR}/build/cscript/libcscript_core.a"
    CACHE INTERNAL "cscript_core static archive")

# osengine itself only links the two .a files for its own .cpp files
# (e.g. cscript_host.cpp pulls Torque-side symbols). PRIVATE so the
# consumer's link line is the authoritative one for ordering.
target_link_libraries(osengine PRIVATE
    ${OSENGINE_LIB3SPACE_A}
    ${OSENGINE_CSCRIPT_CORE_A})

# Platform plumbing — sockets for net code, threading.
if(UNIX)
    target_link_libraries(osengine PUBLIC dl pthread m)
elseif(WIN32)
    target_link_libraries(osengine PUBLIC ws2_32 winmm)
endif()
