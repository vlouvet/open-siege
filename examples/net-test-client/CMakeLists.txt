project(net-test-client)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/../../3space/build/cmake)

find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED)
find_package(glm REQUIRED)
find_package(span-lite REQUIRED)
find_package(pegtl REQUIRED)
find_package(libzip REQUIRED)

add_executable(net-test-client main.cpp ghost_stream.cpp reliable_acks.cpp)
set_property(TARGET net-test-client PROPERTY CXX_STANDARD 17)

target_include_directories(net-test-client PRIVATE
    ${CMAKE_SOURCE_DIR}/../../3space/include)

target_link_libraries(net-test-client PRIVATE
    ${CMAKE_SOURCE_DIR}/../../3space/build/lib3space.a
    nlohmann_json::nlohmann_json
    Boost::Boost
    glm::glm
    nonstd::span-lite
    taocpp::pegtl
    libzip::zip)
