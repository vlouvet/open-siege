// Open Siege adapter shim for Torque 3D's torqueConfig.h (spec 15/02).
//
// Upstream Torque ships engine-config knobs through this file at build time.
// We don't import the whole engine, so this provides the minimum-viable
// configuration needed by platform/platform.h and the VM subset we vendor.
//
// If/when later specs in track 15 widen the vendored subset, defines added
// here should mirror the upstream `Engine/source/torqueConfig.h.in` template
// from the pinned TorqueGameEngines/Torque3D SHA (see LICENSE-Torque3D.md).

#ifndef _TORQUECONFIG_H_
#define _TORQUECONFIG_H_

#define TORQUE_GAME_NAME      "Open Siege cscript host"
#define TORQUE_GAME_VERSION   1000
#define TORQUE_GAME_VERSION_STRING "1.0"

#if defined(__APPLE__)
#  define TORQUE_OS_MAC
#  define TORQUE_OS_POSIX
#elif defined(__linux__)
#  define TORQUE_OS_LINUX
#  define TORQUE_OS_POSIX
#elif defined(_WIN32)
#  define TORQUE_OS_WIN
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#  define TORQUE_CPU_ARM64
#  define TORQUE_64
#elif defined(__x86_64__) || defined(_M_X64)
#  define TORQUE_CPU_X64
#  define TORQUE_64
#elif defined(__i386__) || defined(_M_IX86)
#  define TORQUE_CPU_X86
#endif

// Endianness: ARM64 + x86_64 are little-endian on the platforms we target.
// TORQUE_BIG_ENDIAN intentionally left undefined.

#if !defined(NDEBUG)
#  define TORQUE_DEBUG
#  define TORQUE_ENABLE_ASSERTS
#endif

// Knobs that prune optional Torque subsystems we did NOT vendor. Disabling
// these keeps the include closure inside cscript/ + core/ + platform/.
#define TORQUE_DISABLE_VIRTUAL_MOUNT_SYSTEM
#define TORQUE_DISABLE_MEMORY_MANAGER
#define TORQUE_TOOLS    0
#define TORQUE_PLAYER

// Default TorqueScript file extension. Upstream's CMakeLists.txt sets this
// as a compile define (-DTORQUE_SCRIPT_EXTENSION="tscript"). We override it
// here to "cs" — the Tribes-1 convention — since we're vendoring under
// cscript_core without the upstream CMake driver.
#ifndef TORQUE_SCRIPT_EXTENSION
#  define TORQUE_SCRIPT_EXTENSION "cs"
#endif

#endif // _TORQUECONFIG_H_
