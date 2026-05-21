// Open Siege spec 16/01 — dts-viewer's host-side wiring for libcscript_core.

#include "cscript_host.hpp"

#include "console/console.h"
#include "console/script.h"
#include "console/runtime.h"
#include "console/sim.h"
#include "console/torquescript/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

bool g_initialised = false;

void consoleSink(unsigned int /*level*/, const char* line)
{
    std::printf("[cscript] %s\n", line);
}

} // namespace

namespace dts_viewer::cscript {

void init()
{
    if (g_initialised)
        return;
    Con::init();
    Sim::init();
    Con::addConsumer(&consoleSink);
    // TorqueScriptRuntime's constructor auto-registers itself with
    // Con::registerRuntime(0, this). Skip an explicit re-registration —
    // the second call would trip an AssertFatal in debug builds because
    // gRuntimes[0] is already set.
    (void)studio::content::cscript::TorqueScript::getRuntime();
    g_initialised = true;
    std::printf("[cscript] host initialised — VM ready\n");
}

bool runFile(const std::string& path)
{
    init();
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    Con::evaluate(buf.str().c_str(), false, path.c_str());
    return true;
}

void eval(const std::string& src)
{
    init();
    Con::evaluate(src.c_str(), false, "inline");
}

} // namespace dts_viewer::cscript
