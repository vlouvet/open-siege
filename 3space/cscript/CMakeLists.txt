# cscript — Torque 3D console fork (spec 15/02 build wiring)
#
# This subdirectory hosts a vendored copy of the Torque 3D console subsystem
# (see LICENSE-Torque3D.md). Spec 15/02 stands up the minimum CMake plumbing
# so the VM internals (torquescript/) can be compiled in isolation, and so
# subsequent specs (15/03 namespace rename, 15/06 smoke test) have a target
# to attach to.
#
# Status: EXPERIMENTAL. The full Torque console transitively depends on
# subtrees we have NOT imported (T3D/, gfx/, gui/, persistence/, math/,
# collision/, app/). The carveout below intentionally compiles only files
# whose transitive closure stays inside cscript/. See README.md for the
# measured prune list.
#
# To build:
#   cmake --build build --target cscript_core
#
# The target is marked EXCLUDE_FROM_ALL so a plain `cmake --build` of the
# parent lib3space project is unaffected.

set(CSCRIPT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# --- Files we attempt to compile ------------------------------------------
#
# The transitive include closure of the full torquescript/ VM reaches into
# Engine/source/math/, gfx/, gui/, persistence/, and T3D/ — none of which we
# vendored (the spec was explicit about a VM-only fork, not a full engine).
#
# Spec 15/02 therefore compiles only the LEAF translation units whose
# transitive include closure stays inside what we have vendored
# (cscript/console/ + cscript/core/ + cscript/platform/) plus the four
# shim headers (torqueConfig.h, app/version.h, util/returnType.h,
# math/mathTypes.h). The set is intentionally tiny — it exists as proof
# that the build wiring works end-to-end and produces real object code.
#
# Spec 15/03 (namespace rename) and a later 15/02-followup (math/ import)
# will widen the set as the cascade is resolved.
set(CSCRIPT_VM_SOURCES
        ${CSCRIPT_ROOT}/core/strings/stringFunctions.cpp)

add_library(cscript_core STATIC EXCLUDE_FROM_ALL ${CSCRIPT_VM_SOURCES})
set_property(TARGET cscript_core PROPERTY CXX_STANDARD 17)

target_include_directories(cscript_core PUBLIC
        ${CSCRIPT_ROOT}              # so #include "torqueConfig.h" works
        ${CSCRIPT_ROOT}/console      # so #include "ast.h" from torquescript/ works
        ${CSCRIPT_ROOT}/console/torquescript)

# Torque expects platform + arch defines from torqueConfig.h. The shim at
# cscript/torqueConfig.h sets TORQUE_OS_MAC / TORQUE_CPU_ARM64 / TORQUE_64
# / TORQUE_PLAYER based on compiler-builtin platform macros.
target_compile_definitions(cscript_core PUBLIC TORQUE_PLAYER)

# Quiet Torque-internal warnings — upstream code triggers a flood of
# -Wunused / -Wparentheses on apple-clang 17. These are pre-existing in
# the vendored source, not regressions; treat them like third-party deps.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(cscript_core PRIVATE
            -Wno-everything)
endif()
