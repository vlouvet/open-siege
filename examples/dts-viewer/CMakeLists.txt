project(dts-viewer)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    mission_bounds.cpp)

target_include_directories(dts-viewer PRIVATE
    ${THREESPACE_DIR}/include
    ${SDL2_INCLUDE_DIRS})

target_link_libraries(dts-viewer PRIVATE
    ${THREESPACE_DIR}/build/lib3space.a
    ${SDL2_LIBRARIES}
    OpenGL::GL
    nlohmann_json::nlohmann_json
    Boost::Boost
    glm::glm
    nonstd::span-lite
    taocpp::pegtl
    libzip::zip)
