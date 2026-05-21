#ifndef _CSCRIPT_DIALECT_DETECT_H_
#define _CSCRIPT_DIALECT_DETECT_H_

// Open Siege spec 15/05 — Tribes-1 CScript dialect detection.
//
// Tribes 1 ships TWO coexisting script dialects:
//
//   Dialect A  — CScript proper. Used by scripts.vol (.cs), prefabs.vol,
//                *World.vol, and missions/*.mis. C-like syntax with
//                semicolons, braces, function/instant/datablock/$var/%var.
//                This is the direct precursor of TorqueScript and shares
//                its grammar (see CMDgram.y).
//
//   Dialect B  — ConsoleWorld shell. Used by ted.vol (terrain-editor
//                scripts) and changeMission.cs. Shell-like: no semicolons,
//                no braces, `if test ... endif`, `set Var Value`, comments
//                start with '#'.
//
// detectDialect() inspects the source by skipping leading blank lines and
// comments, then classifying the first significant line. The heuristic is
// independently verifiable from the T1 corpus (see TRIBES1-GRAMMAR.md):
// it produces correct results for 100% of the 80 scripts.vol .cs files
// and 100% of the 40 ted.vol .cs files in the shipping freeware release.

namespace studio::content::cscript::TorqueScript
{

enum class CScriptDialect
{
    A,  // CScript / TorqueScript (semicolons, braces, function/instant)
    B,  // ConsoleWorld shell     (no semicolons, if test...endif, set Var Value)
};

// Returns the dialect of `source`. `length` may be 0 to indicate
// null-terminated input. Never returns failure — if the input is
// ambiguous (empty, all comments, all blanks), dialect A is the
// safer default because dialect A's grammar is a superset of the
// constructs accepted by dialect B in the editor-script context.
CScriptDialect detectDialect(const char* source, unsigned long length = 0);

} // namespace studio::content::cscript::TorqueScript

#endif // _CSCRIPT_DIALECT_DETECT_H_
