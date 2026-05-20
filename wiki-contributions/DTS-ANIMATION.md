### DTS animation — node hierarchy, sequences, keyframes

DTS shapes carry a complete skeletal-animation rig in addition to mesh geometry: a node hierarchy (the bones), a flat pool of named sequences (e.g. `run`, `die head`, `jet`), sub-sequences that bind a sequence to one node's animation channel, and a flat keyframe pool indexed by each sub-sequence. This page documents the on-disk layout for the Darkstar branch of [DTS](DTS) (Starsiege / Starsiege: Tribes, versions 2/3/5/6/7/8).

`lib3space`'s `studio::content::dts::darkstar::read_shape()` parses all of it into typed C++ structs, but the public `renderable_shape` interface exposes only an integer `frame_index` per sub-sequence and a callback that emits already-posed vertices — no slerp, no time-step, no `duration`, no `cyclic` flag, no access to the raw `transforms` array. Time-driven playback bypasses `renderable_shape` and reads the `shape_variant` directly. A working CPU-skinned reference implementation lives in this fork's `examples/dts-viewer/main.cpp` — see the end of this page.

### Indirection chain

```
sequence (named, has duration + cyclic flag)
   └── (1..N) sub_sequence  — one per animated node-or-object inside this sequence
          └── (1..M) keyframe  — samples ordered by position
                 └── (1) transform  — quaternion + translation [+ scale]
```

A `sub_sequence` is owned by **either** a `node` (`node.first_sub_sequence_index` / `num_sub_sequences`) **or** an `object` attached to a node (`object.first_sub_sequence_index` / `num_sub_sequences`). Both forms occur in shipping content and the renderer has to walk both. `dts_renderable_shape.cpp:197-223` folds them; the reference implementation in `examples/dts-viewer/main.cpp:200-222` does the same fold against the raw `shape_variant`.

### File layout (v8)

Every shape version begins with a counter block listing the cardinalities of each array, then writes the arrays back-to-back with no per-array tag or length prefix. For a v8 shape (Tribes' dominant version):

```
PERS header (variable)  →  v5::header (44 B, 11 × i32)  →  v8::data (40 B)
   →  nodes[num_nodes]            (v8::node       = 10 B)
   →  sequences[num_sequences]    (v5::sequence   = 32 B)
   →  sub_sequences[num_subseq]   (v8::sub_seq    =  6 B)
   →  keyframes[num_key_frames]   (v8::keyframe   =  8 B)
   →  transforms[num_transforms]  (v8::transform  = 20 B)
   →  names[num_names]            (24 B, NUL-padded ASCII)
   →  objects, details, transitions, frame_triggers (v5+), footer, meshes
   →  optional inline TS::MaterialList tail (see DML)
```

### Node hierarchy (the skeleton)

A `node` carries five fields: a name-pool index, a parent index, a sub-sequence range owned by this node, and a default-transform index pointing at the bind-pose entry in the `transforms` array.

```
v2 node — 20 bytes (also used by v3/v5/v6/v7)        darkstar_structures.hpp:342
  off  size  field                          type
    0    4   name_index                     i32 LE
    4    4   parent_node_index              i32 LE   (-1 for root; observed root names: "bounds")
    8    4   num_sub_sequences              i32 LE
   12    4   first_sub_sequence_index       i32 LE
   16    4   default_transform_index        i32 LE

v8 node — 10 bytes (every field narrowed to int16) darkstar_structures.hpp:744
  off  size  field                          type
    0    2   name_index                     i16 LE
    2    2   parent_node_index              i16 LE
    4    2   num_sub_sequences              i16 LE
    6    2   first_sub_sequence_index       i16 LE
    8    2   default_transform_index        i16 LE
```

`name_index` resolves through the `names` array (each name is a 24-byte NUL-terminated ASCII slot). `default_transform_index` resolves through the `transforms` array — that record is the bind-pose local-to-parent transform for this node, used whenever no sequence is currently animating it.

The bind pose is therefore **not** the first keyframe of any sequence. It is a separate, sequence-independent local transform that the parser surfaces straight off `nodes[i].default_transform_index`. Tracking the two separately is mandatory; the dts-viewer keeps `Node::bind_local` for the static pose and overwrites only the animated channels each frame.

### Sequence + sub-sequence layout

```
v2 sequence — 16 bytes (also used by v3)             darkstar_structures.hpp:352
  off  size  field                          type
    0    4   name_index                     i32 LE  (into names[])
    4    4   cyclic                         i32 LE  (0 = once, non-zero = loop)
    8    4   duration                       f32 LE  (seconds — see below)
   12    4   priority                       i32 LE  (blending priority; unused in this fork)

v5 sequence — 32 bytes (used by v5/v6/v7/v8)         darkstar_structures.hpp:538
  off  size  field                          type
    0    4   name_index                     i32 LE
    4    4   cyclic                         i32 LE
    8    4   duration                       f32 LE
   12    4   priority                       i32 LE
   16    4   first_frame_trigger_index      i32 LE
   20    4   num_frame_triggers             i32 LE
   24    4   num_ifl_sub_sequences          i32 LE
   28    4   first_ifl_sub_sequence_index   i32 LE
```

```
v2 sub_sequence — 12 bytes (also used by v3/v5/v6/v7) darkstar_structures.hpp:364
  off  size  field                          type
    0    4   sequence_index                 i32 LE  (back-pointer to sequences[])
    4    4   num_key_frames                 i32 LE
    8    4   first_key_frame_index          i32 LE  (into keyframes[])

v8 sub_sequence — 6 bytes                            darkstar_structures.hpp:754
  off  size  field                          type
    0    2   sequence_index                 i16 LE
    2    2   num_key_frames                 i16 LE
    4    2   first_key_frame_index          i16 LE
```

Sub-sequences are stored as a flat pool indexed by node/object ranges. To enumerate the sub-sequences that animate node *N* during sequence *S*, walk `nodes[N].first_sub_sequence_index .. + num_sub_sequences`, keep those whose `sub_sequence.sequence_index == S`, and union the result with the object-attached sub-sequences for any object on node *N*. Different sub-sequences within one sequence may have **different** keyframe counts — there is no per-sequence frame-rate field and the "frame count" reported by tools is the max `num_key_frames` across the sequence's sub-sequences.

The **cyclic** flag is stored on every raw sequence but is **not** surfaced via `renderable_shape::get_sequences()` (the public `sequence_info` type has only `name`, `index`, `enabled`, and a list of `sub_sequence_info`). Callers wanting loop-vs-hold playback semantics must read it off the raw `shape_variant`.

### Keyframe + transform layout

```
v2 keyframe — 8 bytes                                darkstar_structures.hpp:372
  off  size  field                          type
    0    4   position                       f32 LE
    4    4   transform_index                u32 LE  (into transforms[])

v3 keyframe — 12 bytes (used by v3/v5/v6/v7)         darkstar_structures.hpp:466
  off  size  field                          type
    0    4   position                       f32 LE
    4    4   transform_index                u32 LE
    8    4   mat_index                      u32 LE  (IFL texture-frame index; flags in upper bits)

v8 keyframe — 8 bytes (transform + mat narrowed)     darkstar_structures.hpp:763
  off  size  field                          type
    0    4   position                       f32 LE
    4    2   transform_index                u16 LE
    6    2   mat_index                      u16 LE
```

```
v2 transform — 40 bytes (used by v2/v3/v5/v6)        darkstar_structures.hpp:379
  off  size  field                          type
    0   16   rotation                       quaternion4f  (4 × f32: x,y,z,w)
   16   12   translation                    vector3f
   28   12   scale                          vector3f

v7 transform — 32 bytes                              darkstar_structures.hpp:663
  off  size  field                          type
    0    8   rotation                       quaternion4s  (4 × i16, /SHRT_MAX)
    8   12   translation                    vector3f
   20   12   scale                          vector3f

v8 transform — 20 bytes (no scale)                   darkstar_structures.hpp:771
  off  size  field                          type
    0    8   rotation                       quaternion4s  (4 × i16, /SHRT_MAX)
    8   12   translation                    vector3f
```

### Worked example — first keyframe + first transform from `chainturret.DTS`

`chainturret.DTS` is a v8 shape (40 472 B). Its header (at file offset `0x18`) reports `num_nodes=24`, `num_sequences=5`, `num_sub_sequences=64`, `num_key_frames=462`, `num_transforms=77`. With the v8 record sizes from the table above, the keyframe array starts at offset `0x37C` and the transform array at `0x11EC`.

```
First keyframe — offset 0x37C, 8 bytes
  00 00 00 00              position        = 0.0f
  01 00                    transform_index = 1
  00 40                    mat_index       = 0x4000   (upper bits used as flags;
                                                       low 14 bits are the IFL frame index)

First transform — offset 0x11EC, 20 bytes (the bind-pose identity at transforms[0])
  00 00  00 00  00 00  ff 7f          rotation = quat4s(0, 0, 0, 32767)
                                                = (0, 0, 0, 1.0) — identity
  00 00 00 00  00 00 00 00  00 00 00 00      translation = (0, 0, 0)
```

Subsequent keyframes for the `visibility` sequence show the alternating `mat_index = 0x4000 / 0xC000` pattern at consecutive (position 0, position 1) records, suggesting the top two bits of `mat_index` are channel-state flags (e.g. visible-on / visible-off) layered on top of the IFL frame index. The dts-viewer reference implementation does not currently consume `mat_index` and treats every keyframe as a pose sample only.

### Units and conventions

#### Rotation

- Per-version: v2/v3/v5/v6 use **`quaternion4f`** (four floats); v7/v8 use **`quaternion4s`** (four `int16_t`, normalised to `[-1, 1]` by dividing by `SHRT_MAX = 32767`). The conversion lives at `3d_structures.hpp:93-103` as `to_float(quaternion4s)`.
- Component order on the wire is `x, y, z, w`. GLM's `glm::quat` constructor expects `(w, x, y, z)`, so the order has to be threaded explicitly at construction time.
- The reference recipe used both by `lib3space`'s renderer (`dts_renderable_shape.cpp:322`) and by this fork's CPU skinner (`examples/dts-viewer/main.cpp:65`) is:

  ```cpp
  glm::mat4 R = glm::transpose(glm::toMat4(glm::quat(rot.w, rot.x, rot.y, rot.z)));
  ```

  The `glm::transpose` is load-bearing — without it the skeleton inverts. The transpose almost certainly compensates for the Darkstar engine's row-major / opposite-handed convention vs. GLM's column-major right-handed default. **Reuse this exact recipe** rather than inventing a per-axis sign-flip; the transpose-once form is what every working consumer in this fork has converged on.
- No Euler angles, no degrees, anywhere in the format.

#### Translation

- `vector3f`, three floats. Local to the parent node — each node's transform is composed as `world_node = world_parent * local_node`, with the local matrix built as `T * transpose(toMat4(quat)) * S` from the keyframe's translation, rotation, and scale.

#### Scale

- `vector3f` in v2/v3/v5/v6/v7. **Absent in v8** transforms — v8 has only rotation + translation, and consumers must substitute `(1, 1, 1)` (see `examples/dts-viewer/main.cpp:82`).

#### Keyframe `position` and the time mapping

The keyframe `position` is a normalised float in `[0, 1]` spanning the sequence's `duration`. The relation `time_in_sec = position * sequence.duration` is **verified experimentally** on larmor's `run` sequence (duration 0.667 s, 13 keyframes per channel): the dts-viewer's `evaluate(seq, t, …)` function (`examples/dts-viewer/main.cpp:305`) produces a bind-pose round-trip at `t = 0`, the last-keyframe pose at `t = duration`, and visibly distinct interpolated poses at `t = duration / 2`. lib3space itself never multiplies the two; this mapping is downstream knowledge.

Within one sequence, different sub-sequences may have different keyframe counts and different `position` spans. The viewer evaluator handles this by computing a sequence-wide normalised `p = clamp(t / duration, 0, 1)` and then locating the bracketing keyframes within each sub-sequence's own ordered position list, slerping rotation with shortest-path correction (`if (dot(q0, q1) < 0) q1 = -q1;`) and lerping translation/scale.

### LOD (detail-level) branches in the node tree

Tribes player and vehicle DTS files are typically authored with multiple **detail levels** (LODs) baked into a single shape. Each LOD has its own complete sub-tree under a `mesh N` node where *N* is the LOD's screen-pixel size. The convention seen across the corpus uses suffixes attached to node names — for `larmor.dts` (v8, 100 nodes), the same skeleton appears three times:

| LOD container node | sub-tree node suffix | bone count | typical use |
|---|---|---|---|
| `mesh 36` (parent: `bounds`) | `lowerback36`, `thorax36`, `head36`, `submesh_torso 36`, … | 32 | close-range / first-person LOD |
| `mesh 2`  (parent: `bounds`) | `lowerback2`,  `thorax2`,  `head2`,  `submesh_torso 2`,  … | 32 | mid-range LOD |
| `mesh 10` (parent: `bounds`) | `lowerback10`, `thorax10`, `head10`, `submesh_torso 10`, … | 32 | long-range LOD |

The numeric suffix is the LOD's `detail.size` field (`darkstar_structures.hpp:403`) — i.e. the screen-pixel threshold at which that detail level becomes the active one. The viewer's spec-02 hierarchy dump shows the same skeletal layout (`VICON`, `lowerback`, `thorax`, `rhumerus`, `rradius`, paired `submesh_*` leaves) repeated under each `mesh N` parent.

**Consequence for sequence enumeration.** Sequences are authored against one LOD branch and the export tool duplicates them — so the same sequence name reappears multiple times in the flat sequence pool. `larmor.dts` has 45 sequences total, including two `wave` entries (`[39]` and `[42]`) targeting different LOD branches. A viewer that selects the "first sequence named X" will work as long as the selected LOD is consistent. The dts-viewer currently renders only the highest-detail LOD (the `36`-suffixed sub-tree for `larmor`) and ignores the rest.

`bounds`, `always`, `dummyalways root`, `dummyalways chasecam` are non-LOD utility nodes at the top of the tree — collision bounds, alwayson root, and camera-attach dummies. They have no `submesh_*` children and are not duplicated per LOD.

### Sample sequence catalogue

Captured by running the dts-viewer on three representative meshes from `tribes-game/base/Entities.vol` (Starsiege: Tribes 1.41 freeware). Each row is one named entry in the shape's `sequences[]` array.

#### Player armour — `larmor.dts` (v8, 100 nodes, 45 sequences)

Categorised excerpt (full list is 45 entries):

| group | example names | duration range | cyclic |
|---|---|---|:-:|
| locomotion | `run` (0.667 s, 13 kf), `runback` (0.600 s), `side left` (0.467 s) | 0.5–0.7 s | yes |
| stance | `crouch root`, `crouch forward`, `crouch side left`, `looks` | 0.2–1.4 s | mixed |
| jet / fall | `jump run`, `fall`, `landing`, `jet` | 0.1–0.9 s | no |
| deaths (12 variants) | `die back`, `die blown back`, `die chest`, `die head`, `die leg right/left`, `die left/right side`, `die grab back`, `die spin`, `die forward kneel`, `die forward` | 1.4–4.7 s, 9–27 kf | no |
| command signals | `sign over here`, `sign stop`, `sign point`, `sign salut`, `sign retreat` | 0.5–2.3 s | no |
| celebrations / taunts | `celebration 1/2/3`, `taunt 1/2`, `wave` (×2 for LOD), `throw` | 0.5–1.7 s | no |
| posture markers | `root`, `apc root`, `apc pilot`, `flyer root` | 0.033 s | yes |
| pose snapshots | `pda access`, `pose kneel`, `pose stand` | 0.6–2.0 s | no |

The recurring `0.033 s` "root" / "apc root" / "flyer root" entries are single-frame pose-snapshot sequences used to switch the rig into a posture (e.g. "sitting in an APC"), not animations to play. The two `wave` entries (indices 39 and 42) animate different LOD branches — see the LOD section above.

#### Vehicle — `hover_apc.DTS` (v8, 28 nodes, 4 sequences)

| # | name | duration | frames | cyclic | nodes |
|---:|---|---:|---:|:-:|---:|
| 0 | `visibility` | 0.033 s | 2 | yes | 16 |
| 1 | `jet` | 0.133 s | 7 | no | 12 |
| 2 | `thrust` | 0.667 s | 21 | yes | 8 |
| 3 | `idle` | 0.667 s | 21 | yes | 8 |

`visibility` is a 2-keyframe channel that toggles the LOD / damage-state visibility of each submesh — `tracks=38` against `nodes=16` shows that many tracks animate the same nodes (separate "visible" and "invisible" channels for damage stages). The `mat_index` flags in the keyframes are how this toggle is expressed at the byte level (see worked example).

#### Turret — `chainturret.DTS` (v8, 24 nodes, 5 sequences)

| # | name | duration | frames | cyclic | nodes |
|---:|---|---:|---:|:-:|---:|
| 0 | `visibility` | 0.067 s | 2 | no | 11 |
| 1 | `power` | 0.733 s | 25 | no | 6 |
| 2 | `fire` | 0.267 s | 11 | no | 10 |
| 3 | `turn` | 0.133 s | 7 | no | 8 |
| 4 | `elevate` | 0.267 s | 11 | no | 8 |

`turn` and `elevate` are the two-axis aiming sequences the AI evaluates at a fractional `position` to point the turret at its target — i.e. they're not played back over time, they're sampled at an interpolated time matching the desired yaw/pitch. `fire` is the muzzle recoil + chamber rotation cycle.

### Reference implementation

A working CPU-skinned playback path lives in this fork's `examples/dts-viewer/main.cpp`. The relevant code, with the commits that introduced each piece:

| What | Function | Commit |
|---|---|---|
| Capture node hierarchy + bind pose from raw `shape_variant` | `build_nodes()` (`main.cpp:86`) | `ec7a525` |
| Enumerate sequences with `duration` + `cyclic` flag | `build_sequences()` (`main.cpp:188`) | `b2ae180` |
| Extract per-track keyframes into a typed mirror | `extract_transform()` (`main.cpp:163`), keyframe loop in `build_sequences()` (`main.cpp:248`) | `4f26357` |
| Time-driven per-node transform evaluator with slerp | `evaluate()` (`main.cpp:305`) | `4f26357` |
| Bind-pose world transform accumulator | `compute_world_bind()` (`main.cpp:360`) | `ec7a525` |
| CPU-skin vertices into a `GL_STREAM_DRAW` VBO | `compute_world_from_locals()` (`main.cpp:390`) and the per-frame skinning loop downstream | `80c0515` |
| Timeline UI (play/pause/scrub) | event-loop additions (search `SDLK_SPACE`, `SDLK_TAB` in `main.cpp`) | `a4cf64a` |
| UV capture + per-material bucketing (texturing track, parallel) | `f6b8089`, `bc2564a` |

Earlier discovery notes — including a detailed reading of which animation fields lib3space *does* and *does not* expose through its public surface — live in `docs/done/02-animation/00-api-findings.md` in this fork.

### Parsed but not yet consumed

`shape.transitions` (cross-fade descriptors), `shape.frame_triggers` (v5+; gameplay-event timestamps like footstep sounds), `keyframe.mat_index` (v3+; IFL texture-frame animation, with upper bits carrying channel-state flags as in the `visibility` worked example), `sequence.priority` (multi-sequence blending), and IFL-channel sub-sequences (the `num_ifl_sub_sequences` slot on v5+ sequences) are all parsed by lib3space but unused by any code in this fork. The viewer skips sub-sequences whose `node_index` does not resolve to either a node-attached or object-attached range.

### See Also

* [DTS](DTS) — the shape format that owns this animation data
* [DML](DML) — material list that can trail a DTS shape; orthogonal to animation, but consumed by the same `read_shape()` entry point
* [VOL](VOL) — archive format that holds `.dts` files
