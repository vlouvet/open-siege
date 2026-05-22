# cscript — Torque 3D console fork
#
# Spec 15/02 stood up the build wiring with a single proof-of-life TU.
# Spec 15/02b (this revision) widens the compilation set to the full
# script-VM surface — every TU under console/torquescript/, the bulk of
# console/, and the core/ + platform/ runtime — by vendoring the missing
# upstream headers + shimming engine subsystems we don't need.
#
# To build:
#   cmake --build build --target cscript_core
#
# EXCLUDE_FROM_ALL keeps cscript_core out of the default lib3space build.

set(CSCRIPT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Excluded TUs — files in console/ / core/ that pull in engine subsystems
# (GUI, persistence/TAML, T3D/, 3D shape loaders, etc.) we have NOT
# vendored. Their functionality (XML object dump, mission editor save,
# Collada-driven persistence) is not on the VM's critical path.
# ---------------------------------------------------------------------------
set(CSCRIPT_CONSOLE_EXCLUDE
        ${CSCRIPT_ROOT}/console/SimXMLDocument.cpp
        ${CSCRIPT_ROOT}/console/consoleXMLExport.cpp
        ${CSCRIPT_ROOT}/console/engineXMLExport.cpp
        ${CSCRIPT_ROOT}/console/persistenceManager.cpp)

set(CSCRIPT_PLATFORM_EXCLUDE
        # platform.cpp + macMain.mm + POSIXMain.cpp pull in app/mainLoop.h
        # (engine main loop), GFX adapters, and the SDL window manager.
        # We're producing a library, not a standalone game; no main loop.
        ${CSCRIPT_ROOT}/platform/platform.cpp
        ${CSCRIPT_ROOT}/platformMac/macMain.mm
        ${CSCRIPT_ROOT}/platformMac/macPlatform.mm
        ${CSCRIPT_ROOT}/platformPOSIX/POSIXMain.cpp
        ${CSCRIPT_ROOT}/platformPOSIX/POSIXGL.client.cpp)

# core/ exclusions — ogg/vorbis/theora require external libs we haven't
# vendored; zip uses a password macro defined in T3D/. Audio + zip aren't
# script-VM critical.
set(CSCRIPT_CORE_EXCLUDE
        ${CSCRIPT_ROOT}/core/ogg/oggInputStream.cpp
        ${CSCRIPT_ROOT}/core/ogg/oggTheoraDecoder.cpp
        ${CSCRIPT_ROOT}/core/ogg/oggVorbisDecoder.cpp
        ${CSCRIPT_ROOT}/core/util/zip/zipArchive.cpp)

# math/ exclusions — mathUtils.cpp uses Frustum which lives in math/util/
# (we didn't vendor frustum.cpp because its include closure touches gfx).
# Frustum is only used by 3D viewport math, not by anything the script
# VM exercises during evaluation.
set(CSCRIPT_MATH_EXCLUDE
        ${CSCRIPT_ROOT}/math/mathUtils.cpp)

# ---------------------------------------------------------------------------
# Source globs — collect every .cpp under the vendored subtrees, then
# filter the exclude list.
# ---------------------------------------------------------------------------
file(GLOB CSCRIPT_TORQUESCRIPT_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/console/torquescript/*.cpp)
file(GLOB CSCRIPT_CONSOLE_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/console/*.cpp)
file(GLOB_RECURSE CSCRIPT_CORE_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/core/*.cpp)
file(GLOB CSCRIPT_PLATFORM_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/platform/*.cpp)
file(GLOB CSCRIPT_POSIX_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/platformPOSIX/*.cpp)
file(GLOB CSCRIPT_MAC_MM CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/platformMac/*.mm)
file(GLOB CSCRIPT_SDLTHREADS_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/platformSDL_threads/*.cpp)
file(GLOB CSCRIPT_MATH_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/math/*.cpp
        ${CSCRIPT_ROOT}/math/public/*.cpp
        ${CSCRIPT_ROOT}/math/impl/*.cpp)
if(APPLE OR (UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64"))
    file(GLOB CSCRIPT_MATH_NEON_CPP CONFIGURE_DEPENDS
            ${CSCRIPT_ROOT}/math/isa/neon/*.cpp)
    list(APPEND CSCRIPT_MATH_CPP ${CSCRIPT_MATH_NEON_CPP})
endif()
file(GLOB CSCRIPT_THREADPOOL_CPP CONFIGURE_DEPENDS
        ${CSCRIPT_ROOT}/platform/threads/*.cpp)
# cinterface/ pulls in app/mainLoop.h + game-controller deps we don't carry.
# The only symbol the platform layer needs (torque_getexecutablepath) is
# stubbed in stubs/engine_stubs.cpp instead.
set(CSCRIPT_CINTERFACE_CPP "")
set(CSCRIPT_EXTRA_CPP
        ${CSCRIPT_ROOT}/util/messaging/message.cpp
        ${CSCRIPT_ROOT}/stubs/engine_stubs.cpp
        # Spec 17/01 — exec() path resolver. Lives at the cscript/
        # top-level so it isn't bundled with the vendored upstream
        # console/ or core/ directories.
        ${CSCRIPT_ROOT}/script_resolver.cpp)

list(REMOVE_ITEM CSCRIPT_CONSOLE_CPP ${CSCRIPT_CONSOLE_EXCLUDE})
list(REMOVE_ITEM CSCRIPT_PLATFORM_CPP ${CSCRIPT_PLATFORM_EXCLUDE})
list(REMOVE_ITEM CSCRIPT_CORE_CPP ${CSCRIPT_CORE_EXCLUDE})
list(REMOVE_ITEM CSCRIPT_POSIX_CPP ${CSCRIPT_PLATFORM_EXCLUDE})
list(REMOVE_ITEM CSCRIPT_MAC_MM ${CSCRIPT_PLATFORM_EXCLUDE})
list(REMOVE_ITEM CSCRIPT_MATH_CPP ${CSCRIPT_MATH_EXCLUDE})

if(NOT APPLE)
    set(CSCRIPT_MAC_MM "")
endif()

set(CSCRIPT_VM_SOURCES
        ${CSCRIPT_TORQUESCRIPT_CPP}
        ${CSCRIPT_CONSOLE_CPP}
        ${CSCRIPT_CORE_CPP}
        ${CSCRIPT_PLATFORM_CPP}
        ${CSCRIPT_POSIX_CPP}
        ${CSCRIPT_MAC_MM}
        ${CSCRIPT_SDLTHREADS_CPP}
        ${CSCRIPT_MATH_CPP}
        ${CSCRIPT_THREADPOOL_CPP}
        ${CSCRIPT_CINTERFACE_CPP}
        ${CSCRIPT_EXTRA_CPP}
        ${CSCRIPT_ROOT}/lib/tinyxml2/tinyxml2.cpp)

add_library(cscript_core STATIC EXCLUDE_FROM_ALL ${CSCRIPT_VM_SOURCES})
set_property(TARGET cscript_core PROPERTY CXX_STANDARD 17)

target_include_directories(cscript_core PUBLIC
        ${CSCRIPT_ROOT}
        ${CSCRIPT_ROOT}/console
        ${CSCRIPT_ROOT}/console/torquescript
        ${CSCRIPT_ROOT}/lib
        ${CSCRIPT_ROOT}/lib/tinyxml2
        ${CSCRIPT_ROOT}/math/public
        ${CSCRIPT_ROOT}/math/impl)

target_compile_definitions(cscript_core PUBLIC TORQUE_PLAYER)

# SDL2 — needed only for the platformSDL_threads/* mutex + semaphore impl.
# dts-viewer's CMakeLists already resolves SDL2 via find_package; we reuse
# the same lookup so a Homebrew-installed SDL2 satisfies the include path.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_include_directories(cscript_core PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(cscript_core PUBLIC ${SDL2_LIBRARIES})
endif()

# macOS frameworks needed for platformMac/*.mm — Cocoa pulls in
# Foundation + AppKit + the Obj-C runtime. CoreFoundation is the
# host for CFConstantString / _NSConstantString literals.
if(APPLE)
    target_link_libraries(cscript_core PUBLIC
            "-framework Cocoa"
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework IOKit")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(cscript_core PRIVATE -Wno-everything)
endif()

# --- Spec 15/05 dialect-detector test executables --------------------------
add_executable(cscript_dialect_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/dialectDetect.test.cpp)
set_property(TARGET cscript_dialect_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_dialect_test PRIVATE cscript_core)

add_executable(cscript_dialect_corpus EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/dialectCorpus.test.cpp)
set_property(TARGET cscript_dialect_corpus PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_dialect_corpus PRIVATE cscript_core)

# Spec 15/06 — hello.cs smoke test. Calls Con::init() then Con::evaluate()
# on a few canned snippets to prove the VM actually runs end-to-end.
add_executable(cscript_hello_smoke EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/hello_smoke.cpp)
set_property(TARGET cscript_hello_smoke PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_hello_smoke PRIVATE cscript_core)

# Spec 15/07 — VM behavioural test suite. Exercises literals, arithmetic,
# variables, functions, recursion, control flow, instant SimGroup, and
# loads real T1 scripts from scripts.vol.
add_executable(cscript_vm_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/vm.test.cpp)
set_property(TARGET cscript_vm_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_vm_test PRIVATE cscript_core)

# Spec 16/01 — SimObject binding test. Defines a ScriptedEntity subclass
# IN THE TEST TU (so IMPLEMENT_CONOBJECT registration is in the binary,
# not lazy-loaded from libcscript_core.a) and exercises field reflection,
# C++ method call from script, and object lifetime.
add_executable(cscript_binding_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/binding.test.cpp)
set_property(TARGET cscript_binding_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_binding_test PRIVATE cscript_core)

# Spec 16/02 — SimSet / SimGroup container test. Exercises nested group
# construction, getObject/getGroup navigation, add/remove/isMember/clear,
# and SimGroup cascade-delete lifetime.
add_executable(cscript_simgroup_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/simgroup.test.cpp)
set_property(TARGET cscript_simgroup_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_simgroup_test PRIVATE cscript_core)

# Spec 16/03 datablock probe — confirms `datablock` keyword + addField
# (typed field reflection) round-trip values.
add_executable(cscript_datablock_probe EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/datablock_probe.cpp)
set_property(TARGET cscript_datablock_probe PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_datablock_probe PRIVATE cscript_core)

# Spec 16/03 datablock end-to-end — typed datablock subclasses, dialect-A
# transform, and real-corpus baseProjData.cs load.
add_executable(cscript_datablock_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/datablock.test.cpp)
set_property(TARGET cscript_datablock_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_datablock_test PRIVATE cscript_core)

# Spec 16/04 binding macros — DECLARE_CONOBJECT / IMPLEMENT_CONOBJECT /
# DefineEngineMethod / DefineEngineFunction end-to-end smoke.
add_executable(cscript_binding_macros_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/binding_macros.test.cpp)
set_property(TARGET cscript_binding_macros_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_binding_macros_test PRIVATE cscript_core)

# Spec 16/05 Player binding — exercises the same DECLARE_CONOBJECT +
# addField + DefineEngineMethod contract that dts-viewer's shipping
# Player class uses, via a structurally equivalent PlayerTest subclass.
add_executable(cscript_player_binding_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/player_binding.test.cpp)
set_property(TARGET cscript_player_binding_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_player_binding_test PRIVATE cscript_core)

# Spec 16/06 entity bindings — 13 SimObject subclasses covering the
# Track 14 entity types. Test re-defines each as `*Ent` to validate the
# binding contract without dts-viewer link wiring; dts-viewer's actual
# classes smoke-tested via --run-script.
add_executable(cscript_entity_bindings_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/entity_bindings.test.cpp)
set_property(TARGET cscript_entity_bindings_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_entity_bindings_test PRIVATE cscript_core)

# Spec 16/07 bidirectional callbacks — C++ Con::executef(obj, "onEvent",
# ...) reaching script-side `function Class::onEvent(%self, ...) {...}`.
add_executable(cscript_script_callbacks_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/script_callbacks.test.cpp)
set_property(TARGET cscript_script_callbacks_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_script_callbacks_test PRIVATE cscript_core)

# Spec 17/01 exec() + script resolver — absolute paths, missing-file
# warnings, recursive exec via ./, search-path resolution, auto .cs
# extension, real T1 corpus load.
add_executable(cscript_exec_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/exec.test.cpp)
set_property(TARGET cscript_exec_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_exec_test PRIVATE cscript_core)

# Spec 17/02 mission lifecycle — MissionGroup build/teardown, trailing
# exec(), schedule() callback under tick(), 3-mission sequential cycle.
# Mirrors the contract exercised by examples/dts-viewer/mission.cpp.
add_executable(cscript_mission_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/mission.test.cpp)
set_property(TARGET cscript_mission_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_mission_test PRIVATE cscript_core)

# Spec 17/03 HUD bindings — addObjective / setObjectiveState /
# removeObjective / notify / centerPrint / bottomPrint / setMissionState.
# The bindings are re-defined inline (same DefineEngineFunction shape as
# the dts-viewer copy in examples/dts-viewer/hud_bindings.cpp).
add_executable(cscript_hud_bindings_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/hud_bindings.test.cpp)
set_property(TARGET cscript_hud_bindings_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_hud_bindings_test PRIVATE cscript_core)

# Spec 17/04 namespace-qualified function-reference args — `Foo::bar`
# (no parens) as an expression value yields "Foo::bar" string. Used
# by objectives.cs:300-style iteration callbacks.
add_executable(cscript_funcref_test EXCLUDE_FROM_ALL
        ${CSCRIPT_ROOT}/tests/funcref.test.cpp)
set_property(TARGET cscript_funcref_test PROPERTY CXX_STANDARD 17)
target_link_libraries(cscript_funcref_test PRIVATE cscript_core)

