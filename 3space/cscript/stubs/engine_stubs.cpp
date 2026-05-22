// Open Siege spec 15/02b — stubs for engine symbols the script VM
// references but never actually exercises during script execution.
//
// The script VM (compiledEval / codeBlock / parser / runtime) links
// against the broader Torque engine surface — clipboard, dialogs, web
// browser, splash window, taggedString network table, etc. In a
// script-VM-only build (cscript_core + cscript_hello_smoke), those
// features are dead code at runtime.
//
// Signatures must match platform/platform.h exactly — see the function
// declarations at lines 177-374.

#include "platform/platform.h"
#include "platform/typetraits.h"
#include "core/strings/stringFunctions.h"
#include "core/stringTable.h"
#include "math/mPoint3.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

// MathUtils helpers — full mathUtils.cpp is excluded because it pulls in
// Frustum (3D viewport math). RotationF::getDirection needs just the
// scalar getVectorFromAngles helper; reimplement it inline.
namespace MathUtils {
    void getVectorFromAngles(Point3F& vec, float yaw, float pitch)
    {
        vec.x = std::cos(pitch) * std::sin(yaw);
        vec.y = std::cos(pitch) * std::cos(yaw);
        vec.z = std::sin(pitch);
    }
}

// Out-of-line definitions for TypeTraits<F32> static members. Upstream
// puts these in platform.cpp; we excluded that TU because it pulls in
// app/mainLoop.h. The three constants are referenced by ThreadPool's
// templated priority queue.
const F32 TypeTraits< F32 >::MIN  = -F32_MAX;
const F32 TypeTraits< F32 >::MAX  =  F32_MAX;
const F32 TypeTraits< F32 >::ZERO =  0;

// engineAPI globals — single-bit flags that engine init normally sets.
namespace engineAPI {
    bool gIsInitialized     = true;
    bool gUseConsoleInterop = true;
}

// Platform::* — host-OS surface. No-op stubs matching upstream sigs.
namespace Platform {
    void postQuitMessage(const S32) {}
    void forceShutdown(S32 returnValue) { std::exit(returnValue); }
    void outputDebugString(const char* /*fmt*/, ...) {}
    void debugBreak() { __builtin_trap(); }

    void AlertOK(const char*, const char*) {}
    bool AlertOKCancel(const char*, const char*) { return false; }
    bool AlertRetry(const char*, const char*)    { return false; }
    ALERT_ASSERT_RESULT AlertAssert(const char*, const char*)
        { return ALERT_ASSERT_DEBUG; }

    bool openWebBrowser(const char*)        { return false; }
    bool displaySplashWindow(String)        { return false; }
    bool closeSplashWindow()                { return true; }

    const char* getClipboard()              { return ""; }
    bool setClipboard(const char*)          { return false; }

    bool getWebDeployment()                 { return false; }

#if defined(__APPLE__)
    // POSIXFileio.cpp provides these on Linux; stub only on macOS
    StringTableEntry getUserHomeDirectory() { return StringTable->insert(""); }
    StringTableEntry getUserDataDirectory() { return StringTable->insert(""); }
#endif
}

// Tagged-string table — used by the network layer to dedupe strings.
unsigned int GameAddTaggedString(const char* /*s*/) { return 0; }

#if defined(__APPLE__)
// POSIXFileio.cpp provides osGetTemporaryDirectory on Linux
const char* osGetTemporaryDirectory() { return "/tmp"; }
#endif

// Open Siege does not vendor cinterface/ — it pulls in the engine main
// loop which we don't carry. The only cinterface symbol that platformMac
// + platformPOSIX actually link against is torque_getexecutablepath().
// Stub it to return empty string so getExecutablePath falls back to its
// platform-default discovery path.
extern "C" const char* torque_getexecutablepath() { return ""; }

// NetConnection::mErrorBuffer — static member declared in sim/netConnection.h
// but defined in netConnection.cpp (which we don't vendor). simDatablock.cpp
// calls getErrorBuffer() during datablock preload. Provide the definition here.
#include "sim/netConnection.h"
String NetConnection::mErrorBuffer;
