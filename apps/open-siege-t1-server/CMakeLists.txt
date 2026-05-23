project(open-siege-t1-server)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Engine library. add_subdirectory is guarded so a top-level CMakeLists
# (spec 09) can build all apps without duplicating the target.
if(NOT TARGET osengine)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine
                     ${CMAKE_CURRENT_BINARY_DIR}/engine)
endif()

# cscript_core (transitively pulled in by osengine) uses SDL2 for its
# thread/mutex/semaphore primitives — see engine/CMakeLists.txt header
# comment. Even the headless server has to link SDL2 to satisfy those
# refs (never initialised; SDL_Init never called).
find_package(SDL2 REQUIRED)

add_executable(open-siege-t1-server main.cpp)

# Filter out SDL2main from SDL2_LIBRARIES — the server has its own main()
# and doesn't include SDL.h, so the SDL2main.a wrapper would just produce
# an "undefined reference to SDL_main" link error on Windows. We still
# need -lSDL2 (the SDL2 import lib) because cscript_core's mutex.cpp /
# thread.cpp / semaphore.cpp have SDL_CreateMutex / CreateThread / etc
# refs that the linker has to satisfy.
set(SDL2_LIBS_NO_MAIN "${SDL2_LIBRARIES}")
list(FILTER SDL2_LIBS_NO_MAIN EXCLUDE REGEX "(SDL|sdl)2?main")

# Link order: lib3space -> cscript_core -> SDL2 (cscript_core has
# unresolved SDL_* refs that SDL2 must satisfy; ld.bfd single-pass on
# MinGW). Cache vars come from engine/CMakeLists.txt.
target_link_libraries(open-siege-t1-server PRIVATE
    osengine
    ${OSENGINE_LIB3SPACE_A}
    ${OSENGINE_CSCRIPT_CORE_A}
    ${SDL2_LIBS_NO_MAIN})

if(APPLE)
    target_link_libraries(open-siege-t1-server PRIVATE
        "-framework Cocoa" "-framework Foundation"
        "-framework CoreFoundation" "-framework IOKit")
endif()
