// Open Siege spec 15/07 — VM behavioural test suite.
//
// Builds on the hello_smoke harness: initialise the VM once, register the
// TorqueScript runtime + console consumer, then run focused tests against:
//
//   - Literals, arithmetic, variables, scoping  (vm_basics)
//   - Dialect-A T1 constructs (instant, @, function params, returns,
//     conditional, switch, etc.)                  (vm_dialect_a)
//   - Real scripts.vol corpus (serverDefaults.cs, clientDefaults.cs, ...)
//
// Each test uses Con::evaluate() + a registered consumer to capture VM
// output, then asserts that the expected lines are present.
//
// printf-based assertions (no Catch2 / GoogleTest); exit code reflects
// pass/fail. Run via:  ./cscript_vm_test

#include "console/console.h"
#include "console/script.h"
#include "console/runtime.h"
#include "console/sim.h"
#include "console/torquescript/runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace studio::content::cscript;

// ---------------------------------------------------------------------------
// Capture VM console output into a buffer the test bodies can scan.
// ---------------------------------------------------------------------------
static std::string gCapture;

static void captureConsole(unsigned int /*level*/, const char* line)
{
    gCapture += line;
    gCapture += '\n';
}

static void resetCapture() { gCapture.clear(); }

// ---------------------------------------------------------------------------
// Assertion helpers.
// ---------------------------------------------------------------------------
static int gPassed = 0;
static int gFailed = 0;
static std::vector<std::string> gFailures;

static void failHere(const char* desc)
{
    ++gFailed;
    gFailures.emplace_back(desc);
    std::fprintf(stderr, "  FAIL: %s\n", desc);
}

static void passHere(const char* desc)
{
    ++gPassed;
    std::printf("  pass: %s\n", desc);
}

static bool captureContains(const char* needle)
{
    return gCapture.find(needle) != std::string::npos;
}

static void checkEvalOutputContains(const char* script, const char* expected,
                                    const char* testName)
{
    resetCapture();
    Con::evaluate(script, false, testName);
    if (captureContains(expected))
        passHere(testName);
    else
    {
        std::string desc = std::string(testName) + " — expected '" + expected
                         + "' in output, got: <<<" + gCapture + ">>>";
        failHere(desc.c_str());
    }
}

static void checkVar(const char* varName, const char* expected, const char* testName)
{
    const char* got = Con::getVariable(varName);
    if (got && std::strcmp(got, expected) == 0)
        passHere(testName);
    else
    {
        std::string desc = std::string(testName) + " — " + varName + " expected '"
                         + expected + "' got '" + (got ? got : "(null)") + "'";
        failHere(desc.c_str());
    }
}

// ---------------------------------------------------------------------------
// Test bodies.
// ---------------------------------------------------------------------------
static void testBasics()
{
    std::printf("\n[group] vm_basics\n");
    checkEvalOutputContains("echo(\"hello\");",                "hello",            "basic_echo");
    checkEvalOutputContains("echo(1 + 2);",                    "3",                "int_add");
    checkEvalOutputContains("echo(10 - 4);",                   "6",                "int_sub");
    checkEvalOutputContains("echo(3 * 4);",                    "12",               "int_mul");
    checkEvalOutputContains("echo(20 / 5);",                   "4",                "int_div");
    checkEvalOutputContains("echo(10 % 3);",                   "1",                "int_mod");
    checkEvalOutputContains("echo(2.5 + 1.5);",                "4",                "float_add");
    checkEvalOutputContains("echo(\"a\" @ \"b\" @ \"c\");",    "abc",              "string_concat");
    checkEvalOutputContains("$x = 7; echo($x);",               "7",                "global_assign");
    checkEvalOutputContains("$x = 7; $y = $x * 2; echo($y);",  "14",               "global_arith");

    Con::evaluate("$persist = 99;", false, "persist");
    checkVar("persist", "99", "global_persist_after_eval");
}

static void testFunctions()
{
    std::printf("\n[group] vm_functions\n");
    checkEvalOutputContains(
        "function add(%a, %b) { return %a + %b; } echo(add(2, 3));",
        "5",
        "function_definition_and_call");
    checkEvalOutputContains(
        "function greet(%name) { return \"hi \" @ %name; } echo(greet(\"world\"));",
        "hi world",
        "function_string_arg");
    checkEvalOutputContains(
        "function fact(%n) { if (%n < 2) return 1; return %n * fact(%n - 1); } echo(fact(5));",
        "120",
        "function_recursion");
    checkEvalOutputContains(
        "function maxof(%a, %b) { if (%a > %b) return %a; return %b; } echo(maxof(3, 7));",
        "7",
        "function_branch");
}

static void testControlFlow()
{
    std::printf("\n[group] vm_control_flow\n");
    checkEvalOutputContains(
        "if (1 == 1) echo(\"yes\"); else echo(\"no\");",
        "yes",
        "if_else_taken");
    checkEvalOutputContains(
        "if (1 == 2) echo(\"yes\"); else echo(\"no\");",
        "no",
        "if_else_not_taken");
    checkEvalOutputContains(
        "%i = 0; while (%i < 3) { echo(%i); %i = %i + 1; }",
        "0",
        "while_loop_first_iter");
    checkEvalOutputContains(
        "%total = 0; for (%i = 1; %i <= 5; %i = %i + 1) %total = %total + %i; echo(%total);",
        "15",
        "for_loop_sum");
}

static void testInstantAndSim()
{
    std::printf("\n[group] vm_instant_dialect_a\n");
    // The 'instant' alias was added in spec 15/04 — same token as 'new',
    // produces the same SimGroup-derived object. Both must parse, evaluate,
    // and return a non-zero numeric object ID.

    // Verify '@' string concat (T1's hallmark dialect-A operator).
    checkEvalOutputContains("echo(\"a\" @ \"b\");", "ab", "dialect-A: @ concat");

    // 'new SimGroup' — produces a Sim object id.
    resetCapture();
    Con::evaluate("$g = new SimGroup(MyGroupA); echo($g);", false, "instant_new");
    const char* gA = Con::getVariable("g");
    if (gA && gA[0] != '\0' && gA[0] != '0')
        passHere("dialect-A: new SimGroup returns object id");
    else
        failHere("dialect-A: new SimGroup did not return an id");

    // 'instant SimGroup' — same token, must also work.
    resetCapture();
    Con::evaluate("$h = instant SimGroup(MyGroupB); echo($h);", false, "instant_instant");
    const char* gB = Con::getVariable("h");
    if (gB && gB[0] != '\0' && gB[0] != '0')
        passHere("dialect-A: instant SimGroup returns object id");
    else
        failHere("dialect-A: instant SimGroup did not return an id");

    // Objects must have distinct IDs.
    if (gA && gB && std::strcmp(gA, gB) != 0)
        passHere("dialect-A: each 'new'/'instant' yields a distinct id");
    else
        failHere("dialect-A: two SimGroup creations returned same id");
}

static char* readFile(const char* path, long* len_out)
{
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return nullptr;
    std::fseek(fp, 0, SEEK_END);
    long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    char* buf = (char*)std::malloc(len + 1);
    std::fread(buf, 1, len, fp);
    buf[len] = '\0';
    std::fclose(fp);
    *len_out = len;
    return buf;
}

static void testRealScripts()
{
    std::printf("\n[group] vm_real_scripts (from /tmp/scripts/)\n");

    // serverDefaults.cs — sets ~30 globals.
    long len;
    if (char* s = readFile("/tmp/scripts/serverDefaults.cs", &len))
    {
        Con::evaluate(s, false, "serverDefaults.cs");
        std::free(s);
        checkVar("Server::HostName",  "TRIBES Server", "serverDefaults: Server::HostName");
        checkVar("Server::teamName0", "Blood Eagle",   "serverDefaults: Server::teamName0");
        checkVar("Server::MaxPlayers","8",             "serverDefaults: Server::MaxPlayers");
    }
    else
    {
        std::printf("  (skip: /tmp/scripts/serverDefaults.cs not present)\n");
    }

    // clientDefaults.cs — sets $pref::* globals. Note: T1 CScript variables
    // are case-insensitive, so when the file has both:
    //   $pref::ShadowDetailMask = 7;     // line 1
    //   $pref::shadowDetailMask = 0;     // line 5
    // the second assignment overwrites the first. Final value is '0'.
    if (char* s = readFile("/tmp/scripts/clientDefaults.cs", &len))
    {
        Con::evaluate(s, false, "clientDefaults.cs");
        std::free(s);
        checkVar("pref::ShadowDetailMask", "0", "clientDefaults: ShadowDetailMask (case-insensitive override)");
        checkVar("pref::packetRate",       "10","clientDefaults: pref::packetRate");
    }
    else
    {
        std::printf("  (skip: /tmp/scripts/clientDefaults.cs not present)\n");
    }
}

// ---------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/)
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    std::printf("cscript_vm_test: starting up\n");
    Con::init();
    Sim::init();   // Required before any SimObject construction works.
    std::printf("cscript_vm_test: Con + Sim init done\n");
    Con::addConsumer(captureConsole);
    Con::registerRuntime(0, TorqueScript::getRuntime());
    std::printf("cscript_vm_test: runtime registered\n");

    testBasics();
    std::printf("cscript_vm_test: testBasics done\n");
    testFunctions();
    std::printf("cscript_vm_test: testFunctions done\n");
    testControlFlow();
    std::printf("cscript_vm_test: testControlFlow done\n");
    testInstantAndSim();
    std::printf("cscript_vm_test: testInstantAndSim done\n");
    testRealScripts();
    std::printf("cscript_vm_test: testRealScripts done\n");

    std::printf("\n========================================================\n");
    std::printf("cscript_vm_test: %d passed, %d failed\n", gPassed, gFailed);
    if (!gFailures.empty())
    {
        std::printf("failures:\n");
        for (const auto& s : gFailures)
            std::printf("  - %s\n", s.c_str());
    }
    return gFailed == 0 ? 0 : 1;
}
