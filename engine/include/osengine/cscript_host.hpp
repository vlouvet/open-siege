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

    // Spec 17/06 — toggle the Tribes-corpus warning filter. When
    // verbose=true the consoleSink forwards every line; when false
    // (default), `Variable %X referenced before used` and `string
    // always evaluates to 0` messages are swallowed and counted.
    void set_verbose(bool verbose);

    // How many lines have been suppressed since the last call to
    // flush_suppression_count. Used by main.cpp to print a single
    // summary at the end of mission load.
    int flush_suppression_count();
}

#endif // DTS_VIEWER_CSCRIPT_HOST_HPP
