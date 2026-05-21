# Torque 3D — fork provenance

The code under this directory (`open-siege/3space/cscript/`) is a verbatim
copy of three subtrees from the Torque 3D engine, distributed under the MIT
license by GarageGames, LLC.

## Source

- **Repository:** https://github.com/GarageGames/Torque3D
- **Branch:** `development`
- **Pinned commit:** `c669fd4005890d68557103940883da555b295e97`
- **Commit date:** 2022-04-03
- **Forked on:** 2026-05-20 (Track 15 spec 01)

## Subtrees imported (verbatim, no modifications in this spec)

| Source path (upstream)          | Destination path (this repo)              | Files | Bytes |
|---------------------------------|-------------------------------------------|-------|-------|
| `Engine/source/console/`        | `open-siege/3space/cscript/console/`      | 115   | 2.1 M |
| `Engine/source/core/`           | `open-siege/3space/cscript/core/`         | 173   | 1.7 M |
| `Engine/source/platform/`       | `open-siege/3space/cscript/platform/`     | 127   | 1.2 M |

The full upstream `LICENSE.md` is preserved alongside this file as
`LICENSE-upstream-Torque3D.md`.

`core/` and `platform/` are copied in full per the spec's "do NOT cherry-pick"
guidance — removing files later is easier than discovering missing ones at
build time. Subsequent specs in this track (15/02 build-system integration,
15/03 namespace rename) trim the unused files once the actual transitive
include closure is verified by a real build.

## Upstream MIT License

```
MIT License

Copyright (c) 2012-2016 GarageGames, LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
