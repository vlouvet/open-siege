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
