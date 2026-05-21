// Open Siege adapter shim for Torque 3D's app/version.h (spec 15/02).
//
// Upstream's app/version.h exposes engine version constants. The full
// app/ subtree was not vendored (it pulls in main-loop / window / journal
// infrastructure that the VM-only carveout doesn't need). This shim
// satisfies the include in platform/platform.h with a stub version.

#ifndef _APP_VERSION_H_
#define _APP_VERSION_H_

#include "platform/types.h"

inline U32 getVersionNumber()      { return 1000; }
inline const char* getVersionString() { return "1.0"; }
inline const char* getCompileTimeString() { return __DATE__ " " __TIME__; }

#endif // _APP_VERSION_H_
