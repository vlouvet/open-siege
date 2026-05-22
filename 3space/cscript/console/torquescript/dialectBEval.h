#ifndef _CSCRIPT_DIALECT_B_EVAL_H_
#define _CSCRIPT_DIALECT_B_EVAL_H_

// Open Siege spec 15/08 — dialect-B evaluator (long-tail coverage).
//
// Spec 15/05 shipped the dialect detector at 100% accuracy across the
// shipping freeware corpus but deferred the dialect-B execution path:
// 40 of ~42 dialect-B files in the entire corpus are editor scripts
// (ted.vol) that need the ConsoleWorld GUI subsystem to do anything
// meaningful, and the only gameplay-relevant dialect-B file
// (`changeMission.cs`) is four lines of shell.
//
// This evaluator does NOT spin up the editor — instead it walks
// dialect-B source line by line, recognises the small set of
// constructs that produce side effects without GUI dependencies
// (`set Var Value`, `alias Name "value"`, `unalias Name`, comments,
// blank lines), and emits a structured warning for every other
// construct it doesn't bind. The acceptance criterion is "zero runtime
// errors": any unbound feature reports an `[unbound dialect-B
// feature: TOKEN]` message via the supplied sink rather than aborting.
//
// The aliases / variables produced are stored in the supplied
// `DialectBContext` so a caller can feed them into the dialect-A VM if
// it wants. v1 of the evaluator is read-only: nothing in the table is
// consulted by the dialect-A VM yet; the table exists so that a future
// `changeMission` invocation can be a real call.

#include <string>
#include <unordered_map>
#include <vector>

namespace studio::content::cscript::TorqueScript
{

// A binding-side table for the (very few) constructs we honour. Kept
// out of the global VM state so test runs are deterministic and reset
// per invocation.
struct DialectBContext
{
    std::unordered_map<std::string, std::string> vars;     // set NAME VALUE
    std::unordered_map<std::string, std::string> aliases;  // alias NAME "..."
};

// Per-call diagnostics. Errors -> evaluateDialectB returns false.
// Warnings -> always non-fatal; useful for "[unbound dialect-B feature]"
// reports the test pass can summarise.
struct DialectBEvalResult
{
    int  lines_scanned     = 0;
    int  lines_executed    = 0;   // `set` / `alias` / `unalias` / blank / comment
    int  unbound_features  = 0;   // editor commands we can't honour
    bool ok                = true; // false only on truly malformed input
    std::vector<std::string> warnings;
};

// Evaluate a dialect-B source buffer. Always returns; never throws.
// `source` need not be null-terminated when `length` is supplied.
DialectBEvalResult evaluateDialectB(const char* source,
                                    unsigned long length,
                                    DialectBContext& ctx);

} // namespace studio::content::cscript::TorqueScript

#endif // _CSCRIPT_DIALECT_B_EVAL_H_
