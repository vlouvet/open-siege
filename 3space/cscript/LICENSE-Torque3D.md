# Torque 3D — fork provenance

The code under this directory (`open-siege/3space/cscript/`) is a verbatim
copy of three subtrees from the **TorqueGameEngines/Torque3D** community
continuation of the original GarageGames Torque 3D engine. Distributed under
the MIT license, with GarageGames retaining the original copyright notice.

## Source

- **Repository:** https://github.com/TorqueGameEngines/Torque3D
- **Branch:** `development`
- **Pinned commit:** `3661499b33c32c04d14a43bd3724deba05673df8`
- **Commit date:** 2026-05-20
- **Forked on:** 2026-05-21 (Track 15 spec 01)

The TorqueGameEngines fork is the actively maintained continuation; the
original GarageGames/Torque3D repository has not received updates since 2022.
A previous attempt at this spec pulled from GarageGames; the swap was made
once the community fork was identified as the canonical upstream.

## Subtrees imported (verbatim, no modifications in this spec)

| Source path (upstream)          | Destination path (this repo)              | Files |
|---------------------------------|-------------------------------------------|-------|
| `Engine/source/console/`        | `open-siege/3space/cscript/console/`      | 122   |
| `Engine/source/core/`           | `open-siege/3space/cscript/core/`         | 175   |
| `Engine/source/platform/`       | `open-siege/3space/cscript/platform/`     | 112   |

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
