project(dts-viewer)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match cscript_core's build type. Release defines NDEBUG which in turn
# suppresses TORQUE_DEBUG; engineObject.h adds two pointer fields when
# TORQUE_DEBUG is set, shifting SimObject's TamlCallbacks base by 16
# bytes and producing an ODR / vtable-thunk mismatch when SimObject
# subclasses (e.g. Player from spec 16/05) are defined here.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
if(UNIX AND NOT APPLE OR WIN32)
    find_package(GLEW REQUIRED)
endif()

# Track 26 spec 01 — osengine library holds all platform-free code.
# dts-viewer adds the GL/SDL/ImGui/audio sources on top.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine
                 ${CMAKE_CURRENT_BINARY_DIR}/engine)

# (Conan finds inherited from osengine via target_link_libraries below.)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_executable(dts-viewer
    main.cpp
    # Viewer-only renderers (GL):
    interior_renderer.cpp
    skybox_renderer.cpp
    lighting.cpp
    terrain_textures.cpp
    text_renderer.cpp
    screenshot.cpp
    entity_renderer.cpp
    mission_bounds_vis.cpp
    # Editor UI (ImGui):
    imgui_layer.cpp
    asset_browser.cpp
    inspector.cpp
    help_menu.cpp
    viewer_state.cpp
    mission_switcher.cpp
    # HUD + audio (SDL):
    hud.cpp
    wav_loader.cpp
    audio.cpp
    mission_sounds.cpp
    # Asset glue (filesystem):
    asset_resolver.cpp
    asset_cache.cpp
    open_url.cpp
    # Net-protocol client driver (uses sockets but no GL/SDL):
    net_client.cpp
    # Engine-tied script wrappers still in dts-viewer pending split
    # (track 26 spec 02 splits entity_state out of entity_renderer):
    entity_bindings.cpp
    projectile.cpp
    # ImGui itself:
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(dts-viewer PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS})

# Windows MinGW (ld.bfd) uses single-pass static-archive resolution.
# libcscript_core.a (via osengine) references SDL_CreateMutex/Thread/
# Semaphore — but those refs aren't seen until cscript_core itself is
# pulled in, by which time SDL2 has already been scanned. Wrap the
# static libs in --start-group/--end-group so the linker re-scans them
# until all refs resolve.
if(WIN32)
    target_link_libraries(dts-viewer PRIVATE
        "-Wl,--start-group"
        osengine
        ${SDL2_LIBRARIES}
        "-Wl,--end-group"
        OpenGL::GL)
else()
    target_link_libraries(dts-viewer PRIVATE
        osengine
        ${SDL2_LIBRARIES}
        OpenGL::GL)
endif()

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
