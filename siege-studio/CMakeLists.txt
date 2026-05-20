project(siege-studio)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(3space QUIET) # used for standalone builds. Requires lib3space to be packaged.
find_package(wxwidgets REQUIRED)
find_package(imgui REQUIRED)
find_package(SFML REQUIRED COMPONENTS graphics window system)

# Vendored imgui-sfml v2.6.1 (MIT). Compiled inline so we link against
# conan's imgui::imgui rather than imgui-sfml's own ImGui build pipeline.
set(IMGUI_SFML_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui-sfml)

file(GLOB STUDIO_SRC_FILES
        src/*.cpp
        src/content/**/*.cpp
        src/views/*.cpp)
list(APPEND STUDIO_SRC_FILES ${IMGUI_SFML_DIR}/imgui-SFML.cpp)

add_subdirectory(tools)
add_executable(siege-studio ${STUDIO_SRC_FILES})
set_property(TARGET siege-studio PROPERTY CXX_STANDARD 17)
target_include_directories(siege-studio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_SFML_DIR})
target_compile_definitions(siege-studio PRIVATE
        IMGUI_USER_CONFIG="${IMGUI_SFML_DIR}/imconfig-SFML.h")
target_link_libraries(siege-studio
        3space::3space
        wxwidgets::wxwidgets
        imgui::imgui
        sfml::sfml)

if(MSVC)
    target_sources(siege-studio PRIVATE src/siege-studio.rc)
    target_compile_definitions(siege-studio PRIVATE -D_CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX AND NOT APPLE)
    find_package(TBB REQUIRED)
    target_link_libraries(siege-studio TBB::tbb)
endif()

add_custom_command(TARGET siege-studio
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/src/logo.png $<TARGET_FILE_DIR:siege-studio>/logo.png
        )

install(TARGETS siege-studio
        CONFIGURATIONS Debug Release
        RUNTIME DESTINATION bin)

install(FILES src/logo.png DESTINATION bin)
