project(open-siege-t1-client)
cmake_minimum_required(VERSION 3.17)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT TARGET osengine)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine
                     ${CMAKE_CURRENT_BINARY_DIR}/engine)
endif()

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
if(UNIX AND NOT APPLE OR WIN32)
    find_package(GLEW REQUIRED)
endif()

add_executable(open-siege-t1-client main.cpp)

target_include_directories(open-siege-t1-client PRIVATE ${SDL2_INCLUDE_DIRS})

# Link order: lib3space -> cscript_core -> SDL2 (see engine/CMakeLists.txt
# header comment about ld.bfd single-pass on MinGW).
target_link_libraries(open-siege-t1-client PRIVATE
    osengine
    ${OSENGINE_LIB3SPACE_A}
    ${OSENGINE_CSCRIPT_CORE_A}
    ${SDL2_LIBRARIES}
    OpenGL::GL)

if(APPLE)
    target_link_libraries(open-siege-t1-client PRIVATE
        "-framework Cocoa" "-framework Foundation"
        "-framework CoreFoundation" "-framework IOKit")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(open-siege-t1-client PRIVATE dl pthread m GLEW::GLEW)
elseif(WIN32)
    target_link_libraries(open-siege-t1-client PRIVATE
        ws2_32 winmm gdi32 user32 shell32 shlwapi psapi GLEW::GLEW)
endif()
