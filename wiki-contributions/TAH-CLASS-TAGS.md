# TAH ghost-stream class-tag observations

**Status:** survey complete; full decode of each tag's payload deferred.
**Source:** `captures/real-tribes/tah-ref-bidir-20260524.pcap`
(real TAH client ↔ TAH server, Ice/Blood Dagger CTF, ~30s of play).

## Procedure

1. Extract every UDP payload, split by direction
   (`/tmp/tah-ref-pkts/{s2c,c2s}_*.bin`).
2. Pipe each `s2c` payload through `net20::parse_typed_packet` using
   our default class-tag map (which knows only the 5 vanilla
   scope-always tags from our 2026-05-22 capture; everything else
   defaults to `StaticShape`).
3. Tally introductions by class_tag.

Decoder script: `/tmp/tah_burst_decode.cpp` (one-off; archived in the
PR for this spec).

## Findings

- Out of 359 server→client packets, **284 walked the typed-record
  stream cleanly** (rest were rate-update or non-DataPacket frames).
- **411 record introductions** observed across the run.
- **60 distinct class_tags** appeared in introductions, ranging 1..1002.
- Our vanilla `kServerPlayerClassTag = 960` is **not present** in TAH's
  stream. None of TAH's tags overlap with our hardcoded map.
- Every record decoded as `StaticShape` because the default-map
  fallback kicks in. The decoded bytes are therefore meaningless — we
  parsed structure (`shape_layer_block` + `static_shape_body`) but
  whether those fields actually contain the right semantic data
  depends on whether TAH's payload schema for that tag matches our
  `StaticShape` decoder. **It almost certainly does not** for most
  tags.

## Most common tags (likely candidates for prioritising decoder work)

| class_tag | intro count | what it probably is |
|-----------|-------------|---------------------|
| 708       | 58          | the dominant world entity — possibly terrain blocks or trees |
| 640       | 52          | second-most-common static |
| 496       | 29          | another mass static |
| 896       | 21          | matches a tag observed in our 2026-05-22 Groove capture |
| 324       | 18          | mid-frequency static |
| 32        | 14          | small group |
| 65        | 10          | small group |
| 263       | 8           | small group |
| 131       | 7           | small group |
| 20, 40, 384, 527, 512, 128, 376, 96, 262 | 2-6 each | scattered |
| 1002, 979, 833, 800, 768, 765, 758, 738, 727, 704, 679, 632, 615, 575, 529, 514, 480, 468, 428, 416, 391, 345, 333, 320, 313, 299, 287, 275, 256, 219, 172, 165, 160, 140, 139, 127, 109, 100, 76, 53, 34, 31 | 1 each | unique scene objects (flags, turrets, generators, etc.) |

The "unique" tag pool (1-intro count) is likely where Player, Vehicle,
and Item live (one per slot / per session). Without per-tag payload
disambiguation we can't yet say which.

## What this means for spec 14c

The original 14c plan assumed our existing per-class readers
(`read_player_body`, `read_vehicle_body`, etc.) would Just Work
against TAH's stream if we picked the right tag → kind map. That
assumption is wrong for two reasons:

1. **Tag count.** 60 distinct tags vs our 5. TAH ghosts every static
   in the mission as its own datablock-specific class. Our parser
   models 5 classes total.
2. **Payload schemas differ per tag.** Even if we knew which tag is
   Player, the actual Player payload bits are likely structured
   differently from `kServerPlayerClassTag = 960`'s payload (different
   datablock IDs, different field encodings, possibly different bit
   widths).

To actually generate a TAH-valid initial burst we would need:

- A per-tag payload schema (~60 unique schemas to reverse).
- A datablock catalogue matching what TAH's mission preloads
  (probably driven by the mission script + a default datablock pool
  that we'd need to capture and replay).
- The full ack-tracking layer (spec 14a) so TAH knows we received its
  packets and so we can react to its requests for missing data.

Realistic effort: 4-8 weeks of focused work, with significant risk
that some schemas can only be derived from leaked TAH source — at
which point the `docs/CLEAN-ROOM-METHODOLOGY.md` two-agent split
becomes mandatory for the parts of the spec we want to PR upstream.

## Recommendation

- **Ship 14a (ack tracking)** — it's well-scoped (~1 week) and useful
  even outside TAH support (improves our own client's robustness
  too).
- **Stop here on 14c unless TAH compat becomes a top business
  priority.** The cost/benefit doesn't favour 4-8 weeks of payload-
  schema RE when our own client speaks our own protocol natively and
  most of the mini-CTF work is happening there.
- Keep this capture + decoder around (`captures/real-tribes/...`,
  `wiki-contributions/...`) so that if TAH compat is picked back up
  later, the survey work is already done.

## Files

- Capture: `captures/real-tribes/tah-ref-bidir-20260524.pcap` (152kB)
- Extracted packets: `/tmp/tah-ref-pkts/` (recreate from pcap via the
  extraction snippet in commit message)
- Decoder: `/tmp/tah_burst_decode.cpp` (one-off; not checked in)
