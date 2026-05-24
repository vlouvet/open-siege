project(open-siege)
cmake_minimum_required(VERSION 3.17)

if(NOT DEFINED CI)
    set(CI false)
endif()

# Default to Release. cscript_core defines TORQUE_DEBUG iff NDEBUG is unset,
# and an empty CMAKE_BUILD_TYPE flips that on — which shifts SimObject's
# vtable by 16 bytes and surfaces as a "non-virtual thunk to
# SimObject::onTamlCustomRead" link error from any consumer built Release.
# Pinning here keeps every subdirectory in sync.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Track 26 spec 09: opt-in toggles for the upstream wxWidgets-based tools
# (don't build on macOS — see CLAUDE.md) and our reverse-engineered apps
# (dts-viewer + open-siege-t1-{client,server} + net-test-client). Default
# is the upstream behaviour so this CMakeLists stays a drop-in for
# people consuming the original Open Siege repo. macOS / Linux dev for
# this fork: cmake -S . -B build -DOPEN_SIEGE_BUILD_UPSTREAM=OFF
#                                 -DOPEN_SIEGE_BUILD_APPS=ON
option(OPEN_SIEGE_BUILD_UPSTREAM "Build siege-installer/launcher/studio (wxWidgets)" ON)
option(OPEN_SIEGE_BUILD_APPS     "Build dts-viewer + open-siege-t1-{client,server}" OFF)

if (CI MATCHES "^True" OR CI MATCHES "^true")
    message(STATUS "Finding package 3Space for CI build")
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
    find_package(3space REQUIRED)
endif()

if (NOT TARGET 3space::3space)
    message(STATUS "Including 3Space target")
    add_subdirectory(3space)
    add_library(3space::3space ALIAS 3space)
endif()

if(OPEN_SIEGE_BUILD_UPSTREAM)
    add_subdirectory(siege-installer)
    add_subdirectory(siege-launcher)
    add_subdirectory(siege-studio)
endif()

if(OPEN_SIEGE_BUILD_APPS)
    add_subdirectory(engine)
    # dts-viewer still lives under examples/ for now — spec 26/09 envisions
    # moving it to apps/dts-viewer/ but that touches the Windows CI paths
    # and the packaging script, deferred until those are migrated.
    add_subdirectory(examples/dts-viewer)
    add_subdirectory(apps/open-siege-t1-client)
    add_subdirectory(apps/open-siege-t1-server)
    add_subdirectory(examples/net-test-client)
endif()

