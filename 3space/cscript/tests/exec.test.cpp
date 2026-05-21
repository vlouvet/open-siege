// Open Siege spec 17/01 — exec() command + script resolver test.
//
// Drives the new ScriptResolver path through both C++ (directly via
// runScriptFile) and script (`exec("foo");`) entry points. Verifies:
//
//   1. Absolute paths resolve and run.
//   2. exec("nonexistent.cs") logs a warning + returns false WITHOUT
//      crashing the calling script.
//   3. Recursive exec — outer.cs calls exec("./inner.cs"), with
//      inner found relative to outer's directory.
//   4. Search-path resolution — addSearchPath("/tmp/dir") makes
//      exec("foo.cs") resolve there.
//   5. .cs extension is appended automatically (exec("foo")
//      resolves to foo.cs).
//   6. Real T1 corpus — exec("serverDefaults.cs") via a search path
//      pointing at /tmp/scripts and side-effects (globals being set)
//      are visible.

#include "console/console.h"
#include "console/script.h"
#include "console/sim.h"
#include "console/torquescript/runtime.h"
#include "script_resolver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>

using namespace studio::content::cscript;

static int gPassed = 0, gFailed = 0;
static void check(bool c, const char* d)
{
    if (c) { ++gPassed; std::printf("  pass: %s\n", d); }
    else   { ++gFailed; std::fprintf(stderr, "  FAIL: %s\n", d); }
}
static void sink(unsigned int, const char*) {}

// Create a tmp file with `content`, returning its path.
static std::string writeTmpFile(const char* name, const std::string& content)
{
    std::string path = "/tmp/open-siege-exec-test-";
    path += name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    return path;
}

static void clearGlobals()
{
    Con::evaluate("$g1=\"\"; $g2=\"\"; $g3=\"\"; $marker=\"\";",
                  false, "exec.test.clear");
}

int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    std::printf("cscript_exec_test: starting up\n");

    Con::init();
    Sim::init();
    Con::addConsumer(sink);
    Con::registerRuntime(0, TorqueScript::getRuntime());

    // -----------------------------------------------------------------------
    // 1. Absolute-path resolve + load.
    // -----------------------------------------------------------------------
    std::printf("\n[group] absolute-path exec\n");
    auto p1 = writeTmpFile("abs1.cs", "$g1 = \"absolute-ok\";");
    clearGlobals();
    bool ok = ScriptResolver::runScriptFile(p1.c_str());
    check(ok, "runScriptFile returns true for absolute path");
    const char* g1 = Con::getVariable("g1");
    check(g1 && std::strcmp(g1, "absolute-ok") == 0,
          "$g1 set to \"absolute-ok\" by absolute-path exec");

    // -----------------------------------------------------------------------
    // 2. Missing-file is a benign no-op.
    // -----------------------------------------------------------------------
    std::printf("\n[group] missing file\n");
    bool missing = ScriptResolver::runScriptFile("/tmp/open-siege-this-does-not-exist.cs");
    check(!missing, "runScriptFile returns false for missing path");
    // Continuing after a missing file must work — the caller script
    // shouldn't be aborted. Simulate via evaluate.
    clearGlobals();
    Con::evaluate(
        "exec(\"/tmp/open-siege-this-does-not-exist.cs\");\n"
        "$g2 = \"after-missing\";\n",
        false, "exec.test");
    const char* g2 = Con::getVariable("g2");
    check(g2 && std::strcmp(g2, "after-missing") == 0,
          "$g2 still set after a failed exec — no abort");

    // -----------------------------------------------------------------------
    // 3. Recursive exec via "./" — outer pushes its dir, inner resolves
    //    relative to outer.
    // -----------------------------------------------------------------------
    std::printf("\n[group] recursive exec (./inner.cs)\n");
    auto pInner = writeTmpFile("inner.cs", "$g3 = \"inner-ran\";");
    // outer.cs lives in /tmp; inner.cs is /tmp/open-siege-exec-test-inner.cs.
    // We need them in the same directory for "./" resolution. Place both
    // in /tmp explicitly.
    {
        std::ofstream f("/tmp/open-siege-outer.cs", std::ios::binary | std::ios::trunc);
        f << "exec(\"./open-siege-exec-test-inner.cs\");\n"
             "$g3 = $g3 @ \"+outer-ran\";\n";
    }
    clearGlobals();
    ok = ScriptResolver::runScriptFile("/tmp/open-siege-outer.cs");
    check(ok, "outer.cs ran");
    const char* g3 = Con::getVariable("g3");
    check(g3 && std::strstr(g3, "inner-ran") != nullptr,
          "inner.cs ran via ./ relative-to-outer resolution");
    check(g3 && std::strstr(g3, "outer-ran") != nullptr,
          "outer continued after recursive exec");

    // -----------------------------------------------------------------------
    // 4. Search-path resolution.
    // -----------------------------------------------------------------------
    std::printf("\n[group] search-path resolve\n");
    auto pSearch = writeTmpFile("search-target.cs", "$marker = \"search-ok\";");
    (void)pSearch;
    ScriptResolver::addSearchPath("/tmp");
    clearGlobals();
    Con::evaluate("exec(\"open-siege-exec-test-search-target.cs\");",
                  false, "exec.test");
    const char* mk = Con::getVariable("marker");
    check(mk && std::strcmp(mk, "search-ok") == 0,
          "exec resolved via search-path /tmp");

    // -----------------------------------------------------------------------
    // 5. Automatic .cs extension.
    // -----------------------------------------------------------------------
    std::printf("\n[group] .cs auto-extension\n");
    clearGlobals();
    // The file is "open-siege-exec-test-search-target.cs" — pass the name
    // without ".cs" and verify the resolver still finds it.
    Con::evaluate("exec(\"open-siege-exec-test-search-target\");",
                  false, "exec.test");
    mk = Con::getVariable("marker");
    check(mk && std::strcmp(mk, "search-ok") == 0,
          "exec finds file when .cs extension is omitted");

    // -----------------------------------------------------------------------
    // 6. Real Tribes-1 corpus via search-path.
    // -----------------------------------------------------------------------
    std::printf("\n[group] real corpus exec(\"serverDefaults.cs\")\n");
    struct stat st;
    if (::stat("/tmp/scripts/serverDefaults.cs", &st) == 0)
    {
        ScriptResolver::addSearchPath("/tmp/scripts");
        Con::evaluate("exec(\"serverDefaults.cs\");", false, "exec.test");
        const char* hn = Con::getVariable("Server::HostName");
        check(hn && std::strcmp(hn, "TRIBES Server") == 0,
              "exec(\"serverDefaults.cs\") -> Server::HostName == \"TRIBES Server\"");
    }
    else
    {
        std::printf("  (skip: /tmp/scripts/serverDefaults.cs not present)\n");
    }

    std::printf("\n========================================================\n");
    std::printf("cscript_exec_test: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
