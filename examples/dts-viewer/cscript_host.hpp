#ifndef DTS_VIEWER_CSCRIPT_HOST_HPP
#define DTS_VIEWER_CSCRIPT_HOST_HPP

// Open Siege spec 16/01 (dts-viewer integration) — script host shim.
//
// Wires libcscript_core into dts-viewer. Call dts_viewer::cscript::init()
// once at startup; afterwards dts_viewer::cscript::runFile(path) evaluates
// a .cs file, and dts_viewer::cscript::eval(src) evaluates an inline
// string. All output goes through the console consumer that prefixes
// `[cscript]`.
//
// This is the minimum-viable wiring. Track 16/01-followup will migrate
// the existing PlayerState / ItemState / TurretState structs in
// main.cpp to be SimObject-derived so script-driven entity creation
// fully replaces the hardcoded mission_loader paths.

#include <string>

namespace dts_viewer::cscript
{
    // Initialise Con + Sim and install a console consumer that prefixes
    // VM output with [cscript]. Idempotent (no-op on second call).
    void init();

    // Evaluate a .cs file. Returns true on success, false if the file
    // can't be opened. Script-level evaluation errors are reported on
    // the console; the function still returns true.
    bool runFile(const std::string& path);

    // Evaluate an inline source string.
    void eval(const std::string& src);
}

#endif // DTS_VIEWER_CSCRIPT_HOST_HPP
