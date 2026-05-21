# cscript — Torque 3D console fork

This directory hosts a fork of the Torque 3D console-script engine, brought
in so that Tribes 1's 167 shipped `.cs` files can execute under Open Siege.

## Status

| Spec | Description | State |
|------|-------------|-------|
| 15/01 | Initial verbatim copy | **done** (this commit) |
| 15/02 | Build-system integration (`3space/CMakeLists.txt`) | pending |
| 15/03 | Namespace rename → `studio::content::cscript` | pending |
| 15/04 | Tribes-1 syntax delta (`instant`, dropped keywords) | pending |
| 15/05 | Dialect-B `ConsoleWorld` shell | pending |
| 15/06 | Hello-world smoke test | pending |
| 15/07 | VM unit tests | pending |

## Provenance

Forked from the actively maintained TorqueGameEngines community continuation.
See `LICENSE-Torque3D.md` for the upstream source, pinned commit SHA, and
verbatim MIT license text. The contents of `console/`, `core/`, and
`platform/` are unmodified copies from upstream; `git diff` against a fresh
checkout of TorqueGameEngines/Torque3D at the pinned SHA should be empty.

## Modifications policy

Modify in place; do **not** rewrite from scratch. The spec-by-spec adaptation
path (rename namespaces, prune unused files, port the Tribes-1 syntax delta)
keeps the diff against upstream legible and lets us re-sync against future
upstream fixes.

When a file is modified locally, mark it with a header comment of the form:

```cpp
// Modified for Open Siege (spec 15/<NN>) — see git log for details.
```

## Note on subtree breadth

The spec calls for "minimum needed core/ deps." We copied `core/` and
`platform/` in full because (a) console pulls many files transitively, (b)
file discovery during build is more painful than over-inclusion at copy time,
and (c) the spec also says "do NOT cherry-pick." Spec 15/02 prunes the set
based on the actual transitive include closure measured by a real build.

## Not yet usable

Nothing in this directory builds yet. Spec 15/02 wires it into
`3space/CMakeLists.txt`; spec 15/06 lands the first executable smoke test
(`./cscript-host hello.cs`).
