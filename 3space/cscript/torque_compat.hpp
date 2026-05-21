#ifndef _OPEN_SIEGE_TORQUE_COMPAT_H_
#define _OPEN_SIEGE_TORQUE_COMPAT_H_

// Open Siege spec 15/03 — namespace rename compat shim.
//
// The Torque 3D fork in cscript/ originally declared its code in the
// top-level Torque namespace plus several sibling top-level namespaces
// (Con, Sim, Compiler, Platform, Memory, Script, FS, ...). Spec 15/03
// renamed `namespace Torque` to `namespace studio::content::cscript`.
//
// The sibling namespaces (Con, Sim, ...) remain top-level INSIDE the
// vendored source so we don't have to wrap every translation unit.
// External consumers want to reach them as
// `studio::content::cscript::Con::evaluate(...)`, etc. — this header
// publishes namespace aliases that satisfy that path without touching
// each upstream file.
//
// Forward-declare the source namespaces so the aliases below can refer
// to them even when the consumer includes only this header.

namespace Con      {}
namespace Sim      {}
namespace Compiler {}
namespace Platform {}
namespace Memory   {}
namespace Script   {}
namespace FS       {}

namespace studio::content::cscript
{
    namespace Con      = ::Con;
    namespace Sim      = ::Sim;
    namespace Compiler = ::Compiler;
    namespace Platform = ::Platform;
    namespace Memory   = ::Memory;
    namespace Script   = ::Script;
    namespace FS       = ::FS;
}

#endif // _OPEN_SIEGE_TORQUE_COMPAT_H_
