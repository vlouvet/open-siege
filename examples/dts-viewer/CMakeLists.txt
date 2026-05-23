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
if(UNIX AND NOT APPLE OR WIN32)
    find_package(GLEW REQUIRED)
endif()

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

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_executable(dts-viewer
    main.cpp
    asset_resolver.cpp
    interior_renderer.cpp
    mission_loader.cpp
    mission_bounds.cpp
    skybox_renderer.cpp
    lighting.cpp
    terrain_textures.cpp
    hud.cpp
    text_renderer.cpp
    screenshot.cpp
    mission_switcher.cpp
    player_controller.cpp
    physics_capsule.cpp
    wav_loader.cpp
    audio.cpp
    mission_sounds.cpp
    projectile.cpp
    weapons.cpp
    vehicle.cpp
    inv_station.cpp
    entity_renderer.cpp
    asset_cache.cpp
    cscript_host.cpp
    player_simobject.cpp
    entity_bindings.cpp
    mission.cpp
    hud_bindings.cpp
    imgui_layer.cpp
    viewer_state.cpp
    asset_browser.cpp
    inspector.cpp
    open_url.cpp
    help_menu.cpp
    bot_paths.cpp
    ai_player.cpp
    ai_bindings.cpp
    # spec 20/15 — remote-ghost rendering. The four .cpp files below live
    # under examples/net-test-client/ (their original home from specs
    # 20/10, 20/12, 20/14, 20/22). We share the sources rather than
    # duplicating them so net-test-client and dts-viewer stay in lock-step.
    net_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../net-test-client/ghost_stream.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../net-test-client/ghost_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../net-test-client/reliable_acks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../net-test-client/client_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../net-test-client/movecommand.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(dts-viewer PRIVATE
    ${THREESPACE_DIR}/include
    ${THREESPACE_DIR}/cscript
    ${THREESPACE_DIR}/cscript/console
    ${THREESPACE_DIR}/cscript/console/torquescript
    ${THREESPACE_DIR}/cscript/math/public
    ${THREESPACE_DIR}/cscript/math/impl
    ${THREESPACE_DIR}/cscript/lib
    ${THREESPACE_DIR}/cscript/lib/tinyxml2
    ${CMAKE_CURRENT_SOURCE_DIR}/../net-test-client
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS})

target_link_libraries(dts-viewer PRIVATE
    ${THREESPACE_DIR}/build/lib3space.a
    ${THREESPACE_DIR}/build/cscript/libcscript_core.a
    ${SDL2_LIBRARIES}
    OpenGL::GL
    nlohmann_json::nlohmann_json
    Boost::Boost
    glm::glm
    nonstd::span-lite
    taocpp::pegtl
    libzip::zip)

if(APPLE)
    target_link_libraries(dts-viewer PRIVATE
        "-framework Cocoa"
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework IOKit")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(dts-viewer PRIVATE dl pthread m GLEW::GLEW)
    target_compile_definitions(dts-viewer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)
elseif(WIN32)
    target_link_libraries(dts-viewer PRIVATE
        ws2_32 winmm gdi32 user32 shell32 shlwapi psapi GLEW::GLEW)
    target_compile_definitions(dts-viewer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)
endif()
