# Tribes-1 grammar delta (spec 15/04)

The Tribes-1 dialect of CScript predates TorqueScript. Spec 15/04 adapts
the forked Torque 3D grammar to accept the T1 source corpus that ships in
`scripts.vol` (.cs files) and `missions/*.mis` (mission descriptors).

## Delta from Torque 3D → Tribes 1

| Construct | Torque 3D | Tribes 1 | Action |
|---|---|---|---|
| Object construction in scripts | `new SimGroup "name" { ... };` | `new SimGroup "name" { ... };` | unchanged |
| Object construction in .mis | (uses `new`) | `instant SimGroup "name" { ... };` | **add `instant` alias** |
| Switch / case | `switch (e) { case 1: ... }` | not used | silent-accept (grammar kept, dormant) |
| Package | `package PackName { ... };` | not used | silent-accept (grammar kept, dormant) |
| Boolean `==` for strings | dialect-specific | both numeric and string | unchanged (already handled by VM at evaluation time) |
| String concat `@` | yes | yes (heavily) | unchanged |

## Implementation

The only grammar change required is the lexer addition for `instant`:

```l
"new"       { CMDlval.i = MakeToken<int>(rwDECLARE, yylineno); return rwDECLARE; }
"instant"   { CMDlval.i = MakeToken<int>(rwDECLARE, yylineno); return rwDECLARE; }
```

Both keywords emit the same token (`rwDECLARE`), so the existing grammar
production for object construction (CMDgram.y lines ~308-314) handles them
identically. No `.y` change is needed.

`CMDscan.cpp` was regenerated from `CMDscan.l` with flex 2.6.4 (the
toolchain available on macOS as `/usr/bin/flex`). The diff against the
upstream-checked-in file is large because flex's table encoding is
order-dependent, but the only semantic delta is the added `instant` rule
(new case 77 alongside `new`'s case 76).

## Corpus validation (no full parse run yet — VM does not yet compile)

| Claim                                       | Verified by                                  | Status |
|---------------------------------------------|----------------------------------------------|--------|
| All T1 .mis files use `instant`, not `new`  | `grep -c '^\s*instant\s' missions/*.mis`     | ✓ — 42-290 instances per file, 0 `new` |
| No T1 .cs file uses `switch` as keyword     | `grep -nE '^\s*switch\s*\(' scripts.vol/*.cs` | ✓ — 0 matches (only variable / string uses) |
| No T1 .cs file uses `package` as keyword    | `grep -nE '^\s*package\s' scripts.vol/*.cs`  | ✓ — 0 matches |
| `$=` operator absent from T1 corpus         | `grep -nE '\\\$='  scripts.vol/*.cs`         | ✓ — 0 matches |

The parser binary required to actually execute parses (`./cscript-eval
hello.cs`) is blocked behind the math/ subtree import (`Engine/source/math/`)
since `compiler.cpp`, `cmdgram.cpp`, and `consoleInternal.cpp` all pull
math types transitively. The runnable-corpus-parse test from spec 15/04's
acceptance is therefore deferred to spec 15/06 (smoke test), which has
the same VM-runs prerequisite.

## Future revision points

- If a community mod ever ships `switch`/`package` in `.cs`, the dormant
  grammar productions are already present; no further work needed.
- The grammar regen toolchain pinning (bison 3.8.2 + flex 2.6.4) is
  documented here; if upstream resyncs introduce grammar drift, the
  regen flow is `flex -o CMDscan.cpp CMDscan.l` from this directory.

## Dialect B (ConsoleWorld) detection — spec 15/05

Tribes 1 ships TWO coexisting CScript dialects (see CS-SCRIPT.md research
doc). Dialect A is the C-like syntax that became TorqueScript; dialect B
is the shell-style ConsoleWorld syntax used by the terrain editor.

Spec 15/05 implements the **detector** (`dialectDetect.h/.cpp`) — the
file-load entry point that classifies a script as dialect A or B. The
detector is independently testable and ships with two test executables:

| Target                    | Purpose                                                |
|---------------------------|--------------------------------------------------------|
| `cscript_dialect_test`    | 28 canned unit cases covering both dialects + edge cases (block comments, '#' headers, false-positive guards, length-bounded input). |
| `cscript_dialect_corpus`  | Walks a directory of `.cs` files and compares each classification against an expected dialect. Drives the extracted scripts.vol / ted.vol corpora. |

### Corpus verification results

Both runs are 100% accurate (after declaring two documented exceptions
that are actually dialect-B scripts shipped in scripts.vol):

```
$ build/cscript/cscript_dialect_corpus /tmp/scripts A
cscript_dialect_corpus: /tmp/scripts expectation=A scanned=80 matched=80 mismatched=0

$ build/cscript/cscript_dialect_corpus /tmp/ted B
cscript_dialect_corpus: /tmp/ted expectation=B scanned=40 matched=40 mismatched=0
```

The two known dialect-B-in-scripts.vol files are `changeMission.cs`
(4-line shell script) and `loadShow.cs` (alias definitions).

### Classification heuristic (in priority order)

1. **First-line dialect-A leads** — `function`, `datablock`, `singleton`,
   `package`, `switch`, `instant Type`, `new Type`, lines starting with
   `$` or `%`. These shapes never appear at the head of a dialect-B file.
2. **First-line dialect-B leads** — `if test`, `endif`, `set Ident Value`,
   `alias Ident "..."`, `Ted::*` (editor namespace), `not Ident`, plus a
   30-entry whitelist of ConsoleWorld bareword commands observed in
   ted.vol (`newClient`, `focusClient`, `addToolButton`, `confirmBox`,
   `listFiles`, etc.).
3. **Structural brace presence** — dialect-B never uses `{` for blocks,
   so finding any structural `{` (outside strings/comments) proves
   dialect A. This catches T1's implicit-datablock form:
   ```
   SoundProfileData Profile3dVoice
   {
       baseVolume = 0;
       ...
   }
   ```
   which lacks a leading keyword on its first significant line.
4. **Default A** — if no discriminator fires, return dialect A (the
   engine's default load context).

### Dialect-B *parser* — deferred to a follow-up spec

The detector ships in this spec. The full dialect-B **parser** (newline-
significant Flex states + parallel Bison grammar accepting `if test ...
endif`, `set X Y`, bareword calls, namespace-qualified calls like
`Ted::*`) is deferred. Justification:

- Dialect-B is overwhelmingly used by the **terrain editor** (40 of
  ~42 dialect-B files in the entire shipping freeware corpus live in
  ted.vol). The editor is not on the critical path to single-player
  gameplay.
- The only non-editor dialect-B script that gameplay touches is
  `changeMission.cs`, which is 4 lines of shell commands. Once the VM
  is live, that file is small enough to special-case at the load layer
  or hand-translate to dialect A without a full parser.
- The dialect-B parser would require a non-trivial second grammar
  (separate Bison file or parallel rule set) — substantially more code
  than the detector. Deferring keeps Tier-C scope honest.

When a follow-up spec lights it up, the entry point is in place: the
detector returns the dialect; the loader can dispatch to whichever
parser is wired in.
