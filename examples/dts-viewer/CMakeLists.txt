project(dts-viewer)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match cscript_core's build type. Release defines NDEBUG which in turn
# suppresses TORQUE_DEBUG; engineObject.h adds two pointer fields when
# TORQUE_DEBUG is set, shifting SimObject's TamlCallbacks base by 16
# bytes and producing an ODR / vtable-thunk mismatch when SimObject
# subclasses (e.g. Player from spec 16/05) are defined here. See
# discussion in spec 16/05 — caught at link time as missing
# "non-virtual thunk to SimObject::onTamlCustomRead".
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

# 3space + its conan deps. We reuse the Find*.cmake files conan generated
# when we built 3space directly.
set(THREESPACE_DIR ${CMAKE_SOURCE_DIR}/../../3space)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${THREESPACE_DIR}/build/cmake)

find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED)
find_package(glm REQUIRED)
find_package(span-lite REQUIRED)
find_package(pegtl REQUIRED)
find_package(libzip REQUIRED)

add_executable(dts-viewer
    main.cpp
    interior_renderer.cpp
    mission_loader.cpp
    mission_bounds.cpp
    skybox_renderer.cpp
    lighting.cpp
    terrain_textures.cpp
    hud.cpp
    screenshot.cpp
    mission_switcher.cpp
    player_controller.cpp
    physics_capsule.cpp
    wav_loader.cpp
    audio.cpp
    mission_sounds.cpp
    projectile.cpp
    weapons.cpp
    inv_station.cpp
    entity_renderer.cpp
    cscript_host.cpp
    player_simobject.cpp)

target_include_directories(dts-viewer PRIVATE
    ${THREESPACE_DIR}/include
    ${THREESPACE_DIR}/cscript
    ${THREESPACE_DIR}/cscript/console
    ${THREESPACE_DIR}/cscript/console/torquescript
    ${THREESPACE_DIR}/cscript/math/public
    ${THREESPACE_DIR}/cscript/math/impl
    ${THREESPACE_DIR}/cscript/lib
    ${THREESPACE_DIR}/cscript/lib/tinyxml2
    ${SDL2_INCLUDE_DIRS})

target_link_libraries(dts-viewer PRIVATE
    ${THREESPACE_DIR}/build/lib3space.a
    ${THREESPACE_DIR}/build/cscript/libcscript_core.a
    ${SDL2_LIBRARIES}
    OpenGL::GL
    "-framework Cocoa"
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework IOKit"
    nlohmann_json::nlohmann_json
    Boost::Boost
    glm::glm
    nonstd::span-lite
    taocpp::pegtl
    libzip::zip)
