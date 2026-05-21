# cscript — Torque 3D console fork

This directory hosts a fork of the Torque 3D console-script engine, brought
in so that Tribes 1's 167 shipped `.cs` files can execute under Open Siege.

## Status

| Spec  | Description                                              | State    |
|-------|----------------------------------------------------------|----------|
| 15/01 | Initial verbatim copy                                    | **done** |
| 15/02 | Build-system integration (`3space/CMakeLists.txt`)       | **done** (this commit) |
| 15/03 | Namespace rename → `studio::content::cscript`            | pending  |
| 15/04 | Tribes-1 syntax delta (`instant`, dropped keywords)      | pending  |
| 15/05 | Dialect-B `ConsoleWorld` shell                           | pending  |
| 15/06 | Hello-world smoke test                                   | pending  |
| 15/07 | VM unit tests                                            | pending  |

## Build wiring (spec 15/02)

The `cscript_core` static library target is defined in
`cscript/CMakeLists.txt` and registered with the parent `3space/`
CMakeLists via `add_subdirectory(cscript)`. It is marked
`EXCLUDE_FROM_ALL`, so a plain `cmake --build build` of `lib3space` is
unaffected.

To build the cscript target explicitly:

```sh
cd open-siege/3space
cmake -S . -B build -DCMAKE_MODULE_PATH=$(pwd)/build/cmake
cmake --build build --target cscript_core
```

The build emits `build/cscript/libcscript_core.a`. Verify symbols with:

```sh
nm build/cscript/libcscript_core.a | grep ' T '
```

### Compile defines and include paths

Set via `cscript/CMakeLists.txt`:

- `TORQUE_PLAYER` — define visible to all cscript TUs
- Include roots: `cscript/`, `cscript/console/`, `cscript/console/torquescript/`
- Apple-clang gets `-Wno-everything` (upstream Torque code emits many
  warnings; suppressed wholesale rather than per-warning since this is
  third-party vendored source).

### Shim headers added (not in upstream subtrees we vendored)

Spec 01 imported only `console/`, `core/`, `platform/` from upstream.
Several upstream files include headers that live in `Engine/source/`
sibling directories we did **not** import (`app/`, `util/`, `math/`).
Spec 15/02 adds the minimum-viable shims so the include resolver
doesn't fail on the leaf TUs we attempt to build:

| Shim                              | Origin                                                          | Notes |
|-----------------------------------|-----------------------------------------------------------------|-------|
| `cscript/torqueConfig.h`          | hand-written                                                    | Defines `TORQUE_OS_MAC`, `TORQUE_CPU_ARM64`, `TORQUE_64`, `TORQUE_PLAYER`, `TORQUE_DEBUG`, etc. Models upstream's `torqueConfig.h.in`. |
| `cscript/app/version.h`           | hand-written stub                                               | Returns `1000` / `"1.0"` for `getVersionNumber()` / `getVersionString()`. |
| `cscript/util/returnType.h`       | verbatim from `Engine/source/util/returnType.h` @ pinned SHA    | ReturnType template helpers (65 lines). |
| `cscript/math/mathTypes.h`        | verbatim from `Engine/source/math/mathTypes.h` @ pinned SHA     | Math-type forward declarations. Pulls in `console/dynamicTypes.h` but does not pull other math/ headers. |

The pinned SHA is `3661499b33c32c04d14a43bd3724deba05673df8`
(TorqueGameEngines/Torque3D `development`), recorded in
`LICENSE-Torque3D.md`. Files imported verbatim count as part of the
upstream subtree under that same license.

## Compilation set (intentionally narrow)

Spec 15/02 currently compiles **one** translation unit into
`libcscript_core.a`:

- `core/strings/stringFunctions.cpp`

This is the leaf-most string-utility TU we have — its transitive include
closure stays within `core/` + `platform/` + the four shim headers. The
build succeeds, links, and produces symbols (`dPrintf`, `dStricmp`,
`dStrEndsWith`, etc.) that confirm the wiring is end-to-end.

## Why so narrow? — measured prune cascade

The full `torquescript/` VM transitively pulls in headers we have not
vendored. Each blocking header is its own subtree-import task. Measured
order of cascade (rebuilds with `cscript_core` re-globbed across
`console/torquescript/*.cpp`):

1. `app/version.h` ← shimmed (15/02)
2. `util/returnType.h` ← shimmed (15/02)
3. `math/mathTypes.h` ← shimmed (15/02)
4. `math/mPlane.h` ← **next missing**, would need 15/02-followup
5. … additional math/ headers (mPoint2, mPoint3, mPoint4, mMatrix,
    mMathFn, mAngAxis, mBox, mQuat, mRect, mRandom, mTransform,
    mathIO, util/frustum, mPolyhedron) — full list grepped from
    `console/` + `core/` + `platform/`.
6. After math/: persistence/, T3D/, gfx/, gui/ for the wider
   `console.cpp` + `simObject.cpp` files (out of scope for the VM
   carveout — never needed by a pure script interpreter).

A follow-up spec (15/02b, **import minimal Engine/source/math/**) will
either pull in the upstream math/ subtree verbatim (~40 headers) or
write a glm-backed adapter that satisfies the include surface without
re-vendoring 40 TGE-specific math files. The latter is preferred since
`lib3space` already depends on `glm` for the viewer.

## Provenance

Forked from the actively maintained TorqueGameEngines community continuation.
See `LICENSE-Torque3D.md` for the upstream source, pinned commit SHA, and
verbatim MIT license text. The contents of `console/`, `core/`, and
`platform/` are unmodified copies from upstream; `git diff` against a fresh
checkout of TorqueGameEngines/Torque3D at the pinned SHA should be empty.
The four shim headers added in spec 15/02 are listed above; the rest of
the tree remains a clean mirror of upstream.

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
and (c) the spec also says "do NOT cherry-pick." Spec 15/02 measured the
actual transitive closure (above) and confirms that, contrary to the
spec's original 1-2 day estimate, getting the full console subsystem to
compile additionally requires math/, persistence/, gfx/, gui/, T3D/, and
app/ subtree imports. Those are tracked as follow-up specs.
