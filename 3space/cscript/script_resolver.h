#ifndef _OPEN_SIEGE_SCRIPT_RESOLVER_H_
#define _OPEN_SIEGE_SCRIPT_RESOLVER_H_

// Open Siege spec 17/01 — script path resolution for exec().
//
// The vendored Torque runtime.cpp / executeFile path uses the full
// volume-mount + DSO-cache machinery (FS::GetFileNode, expandScriptFilename,
// prefs path, journaling, ...). Our cut-down build doesn't carry those
// subsystems, so exec("foo.cs") returns false silently.
//
// This header exposes a simpler resolver: a vector of search directories
// plus a per-call "current script dir" stack that handles relative paths
// like `exec("./gameplay/ctf.cs")`. Tribes scripts use both forms.
//
// The resolver is wired into `runScriptFile()` which loads + evaluates
// the resolved file via Con::evaluate. The exec ConsoleFunction in
// consoleFunctions.cpp now calls runScriptFile() instead of
// Con::executeFile().

#include <string>
#include <vector>

namespace studio { namespace content { namespace cscript { namespace ScriptResolver {

// Add a directory to the search list. Later-added paths are searched
// after earlier ones. Idempotent on duplicates.
void addSearchPath(const std::string& dir);

// Clear all search paths. Used by tests; production code shouldn't need it.
void clearSearchPaths();

/// Resolve `scriptPath` against the current-script-dir stack + search
/// paths. Returns the absolute path that exists on disk, or an empty
/// string if no candidate file was found.
///
/// Resolution rules (mirroring Torque convention):
///   1. If `scriptPath` is absolute, use it as-is.
///   2. If it starts with "./", resolve relative to the most-recently
///      pushed script directory (top of stack).
///   3. Otherwise, try each search path in order.
///   4. If still no match, try the same candidates with ".cs" appended.
std::string resolve(const std::string& scriptPath);

/// Load + Con::evaluate the file at `scriptPath`. Returns true on
/// success, false if the file couldn't be resolved or read.
/// Pushes the script's parent directory onto the current-dir stack
/// for the duration of evaluation so nested exec("./...") calls
/// resolve correctly.
bool runScriptFile(const char* scriptPath);

// Test/diagnostic helpers.
std::vector<std::string> currentSearchPaths();
int  currentStackDepth();

}}}} // namespace studio::content::cscript::ScriptResolver

#endif
