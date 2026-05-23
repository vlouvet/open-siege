project(net-test-client)
cmake_minimum_required(VERSION 3.17)

# Track 26 spec 01 — net-protocol sources moved to engine/ (libosengine.a).
# net-test-client is now a thin wrapper that links osengine for the
# ghost_stream / reliable_acks / client_events / movecommand code, and
# adds the CLI subcommand dispatch in main.cpp.

# Bring in osengine if not already added by the parent project. When
# net-test-client is built standalone, this adds the library; when built
# from a top-level CMake that also pulls in apps/dts-viewer (which
# add_subdirectory's engine first), CMake dedupes silently.
if(NOT TARGET osengine)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine
                     ${CMAKE_CURRENT_BINARY_DIR}/engine)
endif()

add_executable(net-test-client main.cpp)
set_property(TARGET net-test-client PROPERTY CXX_STANDARD 17)

# Even though net-test-client never calls SDL_*, cscript_core's mutex/
# thread/semaphore code (transitively in osengine) does — see
# engine/CMakeLists.txt header comment. Link SDL2 for symbol resolution.
find_package(SDL2 REQUIRED)
target_link_libraries(net-test-client PRIVATE
    osengine
    ${OSENGINE_LIB3SPACE_A}
    ${OSENGINE_CSCRIPT_CORE_A}
    ${SDL2_LIBRARIES})
