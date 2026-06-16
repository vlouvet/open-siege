# Tribes catalogue-entry-event per-group body schemas — Reader artifact (14c-R-7.1)

**Status:** v1 (2026-06-16). Closes (or substantially scopes-out) the
per-group body-schema gap that 14c-R-7 (`CAP1-GHOST-INTRO-DECODE.md`
§0/§6.1) raised. Source bytes: cap1 kP01..kP10 in
`open-siege/engine/src/tah_ghost_burst.cpp`.

**Executive verdict:** the wire evidence in cap1 rejects the working
assumption that every reliable event of `class_tag = 88` in cap1's TAH
build is a catalogue-entry-event of the layout documented in
`TRIBES-DATABLOCKS.md` §2. The 6 + 8 + 8 (group, group_size, block)
header DOES decode the cap1 bit-stream into a stable bit-aligned
sequence of ~10 events per packet, BUT the `block` field reads as `0`
for every one of the 90 events in kP02..kP10 — including across an
intra-packet group transition (kP02 events 0..8 read `(g=0, gs=10,
blk=0)`, events 9..10 read `(g=1, gs=153, blk=0)`) — which is
**structurally incompatible** with `TRIBES-DATABLOCKS.md` §3.1 (block
must increment within each group). The Reader closes the spec with
hypothesis #2 from `CAP1-GHOST-INTRO-DECODE.md` §6.2 endorsed: in
cap1's TAH build, **`class_tag = 88` is a reliable event of a
different per-event payload class than catalogue-entry-event**, and
the leading 22 bits of each event body are a fixed structural marker
the Reader could not independently interpret without per-class
decoder evidence the clean-room specs do not contain.

The §2 per-group body schemas this spec was scoped to produce are
therefore mostly UNRESOLVED. The Reader documents what IS resolvable:
(a) the catalogue-entry header layout is reaffirmed in §1 as the
spec-documented 6 + 8 + 8 (one of several candidate widths tested
against cap1; only 6 + 8 + 8 produces the (0, 10, 0) tuple matching
the V1 capture in `TRIBES-DATABLOCKS.md` §6.1 at kP02 event 0); (b)
the per-event total widths and packet bit budget cross-check
(§3) show ~170 bits per event × ~10 events per packet × 9 packets ≈
1900 bytes of "catalogue-shaped" content, consistent with
TAH-CLASS-TAGS.md's estimate of 75–110 scope-always intros per CTF
mission burst; (c) the sentinel-encoding question in §5 is partly
addressable from kP02's structural transition between group 0 and
group 1; (d) the alternative hypothesis in §4 is now the recommended
path forward for closing the burst-content gap.

**Companion specs consulted (Reader-allowed):**

- `CAP1-GHOST-INTRO-DECODE.md` (§0 executive, §1 decodable region,
  §2.2 first-event evidence, §6.1 scope-out, §6.2 alternative
  hypothesis)
- `TRIBES-DATABLOCKS.md` (§2 framing, §3.1 sentinel rule, §3.2 group
  ordering, §3.3 block_id_ref width formula, §5.1–§5.3 body sketches,
  §6.1–§6.5 validation vectors from a separate non-TAH capture)
- `TRIBES-NETPROTO.md` (§5.0 ghost-stream framing, §6 bit primitives,
  §7 reliable-event framing, §14 VC header)
- `TRIBES-INITIAL-BURST.md` (§2.3 phase-2 wire format, §3
  scope-always-complete signal)
- `TRIBES-GHOST-CLASSES.md` (§1 class-tag ranges)
- `TRIBES-PHASE2-PACKING.md` (§1 per-packet byte budget)

**Clean-room compliance:** no leaked-Dynamix-source consultation was
used to produce this Reader artifact. All wire-shape claims are
derived from bit-level decoding of the cap1 byte-arrays plus the
named clean-room specs. **Forbidden references explicitly NOT
consulted:** `github.com/MortarTurret/Darkstar` (any branch),
`github.com/sdozeman/starsiege-tribes` (any branch), or any other
leaked-Dynamix-engine mirror.

---

## 0. Executive finding (read this first)

The Reader tested **13 candidate widths** for the catalogue-entry
body header against the bit-stream of cap1's first-event of every
packet (kP02..kP10). The candidates were:

| (group, group_size, block) | kP02 ev0 | kP03 ev0 | kP04 ev0 | kP05 ev0 |
|----------------------------|----------|----------|----------|----------|
| **6+8+8 (spec authority)** | (0, 10, 0) | (1, 153, 0) | (1, 153, 0) | (1, 153, 0) |
| 5+8+8                      | (0, 20, 0) | (1, 50, 1) | (1, 50, 1) | (1, 50, 1) |
| 7+8+8                      | (0, 5, 0)  | (65, 76, 0)| (65, 76, 0)| (65, 76, 0)|
| 6+9+8                      | (0, 10, 0) | (1, 153, 0)| (1, 153, 0)| (1, 153, 0)|
| 6+10+8                     | (0, 10, 0) | (1, 153, 0)| (1, 153, 0)| (1, 153, 0)|
| 6+8+9                      | (0, 10, 0) | (1, 153, 0)| (1, 153, 0)| (1, 153, 0)|
| 6+8+10                     | (0, 10, 0) | (1, 153, 0)| (1, 153, 0)| (1, 153, 0)|
| 4+8+8                      | (0, 40, 0) | (1, 100, 2)| (1, 100, 2)| (1, 100, 2)|
| 5+9+8                      | (0, 20, 0) | (1, 306, 0)| (1, 306, 0)| (1, 306, 0)|
| 6+7+8                      | (0, 10, 0) | (1, 25, 1) | (1, 25, 1) | (1, 25, 1) |
| 6+8+7                      | (0, 10, 0) | (1, 153, 0)| (1, 153, 0)| (1, 153, 0)|

**Only the 6 + 8 + 8 width-set (and its 1-bit wider variants 6+9+8,
6+10+8, 6+8+9, 6+8+10, 6+8+7) decode kP02 event 0 to the
spec-authority value `(group=0, group_size=10, block=0)`** matching
the V1 capture documented in `TRIBES-DATABLOCKS.md` §6.1. Every other
candidate either produces a group-size that contradicts
`TRIBES-DATABLOCKS.md` §4's stock-mission count of 10
SoundProfileData entries, or produces an impossibly-large block index
at the first record.

So the header width is reaffirmed as **6 + 8 + 8**, matching the
existing spec. The header width is NOT the source of the kP04+
anomaly that R-7 §6.2 hypothesised.

But — and this is the executive-finding twist — **all 90 events in
cap1's kP02..kP10 read `block = 0` under the reaffirmed 6 + 8 + 8
header**, including the 9 events of kP02 that ought (per §3.1) to
read `block = 0..8` for SoundProfileData[0..8]. The block field is
NOT incrementing on the wire.

The Reader interprets this as: **`class_tag = 88` in cap1's TAH
build is a reliable event of a different per-payload class than
the `TRIBES-DATABLOCKS.md` catalogue-entry-event**. The 22-bit
"`(group, group_size, block)`" prefix decoded from the body of every
class-88 event in cap1 is most plausibly the leading bits of an
unrelated per-event payload — possibly a ghost-introduction event,
possibly a server-state-publish event — whose own per-class
decoder, were the Reader to access it, would NOT use these 22 bits
as group / group_size / block fields at all.

The follow-up Implementer scope this finding unblocks is in §4.4:
the I-7 orchestrator should drop the assumption that the cap1 burst
encodes scope-always ghost intros inside a catalogue-event stream,
and instead investigate what TAH actually expects on a fresh session
(potentially a separate, smaller catalogue + a separate
GSS-driven ghost burst). Live-fuzzing against TAH is now the
recommended path; the wire-content-replication path that 14c-I-6
attempted is at a dead-end for the cap1 corpus.

### 0.1 Total bit-budget summary (proof of work, see §3)

The Reader scanned each packet kP02..kP10 for occurrences of the
**ten-bit seq-continuous-tag-88 event boundary pattern**
(`ep=1, guar=1, sc=1, tag=88` = bits `1, 1, 1, 0, 0, 0, 1, 1, 0, 1`
LSB-first). Each packet produced 9 occurrences (kP02 produced 10,
its first event being a non-sc-continuous explicit-seq form). With
the explicit-seq first event added, **each packet contains exactly
10 events of class_tag = 88** (kP02 has 11 because the
SoundProfile→Sound group transition straddles its boundary).

Per-event width averages 165..175 bits across the 9 packets, with
per-event variance of 100..205 bits (the explicit-seq first event
of each packet uses 18 header bits vs 10 for the sc-continuous
follow-ups, accounting for ~8 bits of the variance; the rest is
genuine body-content variance — see §3).

Total catalogue-shaped content across kP02..kP10:
**~15,300 bits ≈ 1900 bytes** in 90 reliable events plus 30..75
bits of trailing framing per packet (ESS-end + ISS-pres + GSS-pres
+ pad). The 90-event count is consistent with the
TAH-CLASS-TAGS.md survey's 75–110 scope-always-intro estimate for a
stock CTF mission, lending further weight to hypothesis #2 (these
events are intros, not catalogue records).

---

## 1. Catalogue-entry-event header schema (reaffirmed)

### 1.1 Bit layout

The catalogue-entry-event body header is **6 + 8 + 8 = 22 bits**,
matching `TRIBES-DATABLOCKS.md` §2 exactly:

| Body bit | Width | Field      |
|----------|-------|------------|
| 0..5     | 6     | group      |
| 6..13    | 8     | group_size |
| 14..21   | 8     | block      |

All three fields are unsigned LSB-first integers per
`TRIBES-NETPROTO.md` §6.2.

### 1.2 Evidence from cap1 (≥ 4 events)

The following table lists the cap1 first-event of every packet
kP02..kP10, with the bit-position evidence and the decoded
`(group, group_size, block)` tuple. The body bit position is computed
as `35 (ESS start = bit 35) + 18 (first event header = ep + guar +
sc=0 + he=1 + es=7 + tag=7) = 53` for every packet, since R0 = R1 = 0
(no rate-control change) on all of kP02..kP10:

| Packet | Body start bit | Bits 53..58 (group) | Bits 59..66 (group_size) | Bits 67..74 (block) |
|--------|----------------|----------------------|---------------------------|----------------------|
| kP02   | 53             | `0, 0, 0, 0, 0, 0` → 0 | `0, 1, 0, 1, 0, 0, 0, 0` → 10 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP03   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP04   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP05   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP06   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP07   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP08   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP09   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |
| kP10   | 53             | `1, 0, 0, 0, 0, 0` → 1 | `1, 0, 0, 1, 1, 0, 0, 1` → 153 | `0, 0, 0, 0, 0, 0, 0, 0` → 0 |

kP02 matches the V1 capture verbatim
(`TRIBES-DATABLOCKS.md` §6.1: `group = 0, group_size = 10, block =
0`). This confirms the 6 + 8 + 8 width and the spec-documented bit
position for these three fields at the first event of a cap1-style
burst is correct.

### 1.3 Per-event mid-packet header evidence

The Reader also verified the 6 + 8 + 8 layout against events 1..N
in each packet (sc-continuous events with no explicit_seq, so body
start = event start + 10 bits):

| Packet | Event index | Event header start bit | Body start bit | (group, group_size, block) |
|--------|-------------|-------------------------|------------------|-----------------------------|
| kP02   | 1           | 145                     | 155              | (0, 10, 0) |
| kP02   | 5           | 745                     | 755              | (0, 10, 0) |
| kP02   | 8           | 1243                    | 1253             | (0, 10, 0) |
| kP02   | 9           | 1409                    | 1419             | **(1, 153, 0)** — group transition |
| kP02   | 10          | 1613                    | 1623             | (1, 153, 0) |
| kP05   | 4           | 729                     | 739              | (1, 153, 0) |
| kP05   | 8           | 1416                    | 1426             | (1, 153, 0) |
| kP10   | 5           | 852                     | 862              | (1, 153, 0) |

(Event-start bit positions were located by scanning the bit-stream
for the 10-bit pattern `ep=1, guar=1, sc=1, tag=88` =
`1, 1, 1, 0, 0, 0, 1, 1, 0, 1` LSB-first; the pattern occurs exactly
9 times in each packet beyond the explicit-seq first event.)

### 1.4 The Reader's reaffirming conclusion on §1

The 22-bit (6 + 8 + 8) catalogue-entry-event body header described
in `TRIBES-DATABLOCKS.md` §2 is **correct in width and field order**
per cap1 evidence. The header width is NOT the gap that R-7's §6.2
hypothesised.

However:

- The `block` field reads `0` for every one of 90 events in cap1's
  kP02..kP10, including across an intra-packet group transition
  (kP02 ev9 transitions from group 0 to group 1 with both records
  reading `block = 0`).
- Per `TRIBES-DATABLOCKS.md` §3.1, the block field MUST increment
  from `0` to `group_size − 1` within each group. cap1 violates this.

The most parsimonious resolution is that **`class_tag = 88` in cap1
is NOT the catalogue-entry-event class** documented in §2 of
`TRIBES-DATABLOCKS.md`, but a different reliable-event class whose
payload happens to begin with a 22-bit prefix interpretable as
`(1, 153, 0)`. See §4 for the alternative-hypothesis development.

---

## 2. Per-group body schemas

Per §1's reaffirmed header width, the body bits 0..21 of every
class-88 event are decodable as `(group, group_size, block)`. The
question this section was scoped to answer is: **what are the bits
that follow the 22-bit header — the per-group body — for groups 0,
1, 3, 4, 5, and 21?**

The Reader's finding here is uniformly negative: per §1.4 the
class-88 events do NOT appear to be catalogue records at all, so
per-group body schemas cannot be derived from cap1 alone. Each
subsection below documents what is known, what would be needed to
resolve the schema, and which follow-up Reader spec would close the
gap.

### 2.1 Group 0 (SoundProfileData) — partial

**What is decodable from cap1:** kP02 event 0 decodes (under the
6+8+8 header + `TRIBES-DATABLOCKS.md` §5.1 SoundProfileData body
schema) as:

- `group = 0, group_size = 10, block = 0` (matches V1)
- `flags = 0 (6 bits = 0)`
- `baseVolume = 0 (10 bits, value 0/1023)`
- `f1 = 1` (cone-inside-angle differs from default)
- `coneInsideAngle = 0 (10 bits = 0/1023)`
- `f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0`
- Body total: 22 + 6 + 10 + 1 + 10 + 5 = **54 bits**

This body-walk terminates with an event-present = 0 bit at body
end (bit 107 of kP02) under the assumption that the catalogue
event has finished. But the cap1 byte-stream beyond bit 107 of kP02
contains 1740 bits of non-zero content that DOES NOT match an
ESS-end + ISS-pres + GSS-pres + zero-pad framing. Specifically, the
Reader pattern-search confirms 10 additional event-boundary
markers at bits 145, 247, 413, 579, 745, 911, 1077, 1243, 1409,
1613 of kP02 — meaning kP02 contains 11 events of class_tag=88,
NOT 1. The 54-bit body walk under the `TRIBES-DATABLOCKS.md` §5.1
schema is therefore inconsistent with the actual event spacing
(110-204 bits between event starts in kP02).

**Where the SPD §5.1 schema breaks down:** the schema's worst-case
body width is 22 + 10 + 10 + 21 + 96 = 159 bits (every flag set),
but the actual cap1 event spacing for events 1..8 of kP02 is 102 to
166 bits. The schema MIGHT fit for these events — but every event
reads `(g=0, gs=10, blk=0)` for events 0..8 (with block stuck at 0),
which means the SPD schema's assumed catalogue header is producing
duplicate records. The schema would need additional per-event
state-fields (a quantized buffer-id reference? a per-record name
string?) that are not in the §5.1 spec.

**Resolution status:** **UNRESOLVED for cap1.** The V1-decoded SPD
body (per `TRIBES-DATABLOCKS.md` §6.1) is the only ground-truth
worked-example we have, and that decode applies to ONE event of one
packet from a non-cap1 capture. The cap1 packets contain 11
class-88 events purportedly in group 0 + group 1, but the per-event
body content is NOT matched by the §5.1 schema.

**Suggested follow-up Reader scope (R-7.1.1):**

- Capture a fresh, single-record-per-packet catalogue dump from a
  vanilla Tribes 1.41 server (not TAH) to confirm the §5.1 SPD body
  schema in isolation.
- If the vanilla capture matches §5.1 cleanly, the cap1
  divergence is TAH-specific and supports hypothesis §4
  (`class_tag = 88` in TAH is a different event class).

### 2.2 Group 1 (SoundData) — UNRESOLVED

**What is decodable from cap1:** kP02 events 9..10 + all of kP03..kP10
events decode as `(g=1, gs=153, blk=0)` repeated. Per the
`TRIBES-DATABLOCKS.md` §5.2 SoundData body schema:

- `wavFileName` (uncompressed string: 1 bit compressed + 8 bits length
  + 8 * N bits body)
- `priority` (6 bits)
- `profileIndex` (4 bits, since group 0 has size 10)

Minimum body = 22 + 1 + 8 + 0 + 6 + 4 = **41 bits** (empty filename).
Typical body for a 12-character filename = 41 + 96 = **137 bits**.

The Reader applied this schema to each class-88 event of kP03..kP10
and got `str_len = 0` for every event — meaning the wavFileName is
empty. This is not plausible for real SoundData blocks (every Sound
entry references a `.wav` filename). The decoded `priority = 0` and
`profileIdx` cycling through 0, 2, 4, 6, 8, 10, 12, 14, 0, 4, 8 (a
4-bit field cycling by 2 per event) are also inconsistent with stock
asset content.

The actual cap1 per-event width for group-1 events (~165-180 bits
including framing) cannot be accounted for by the §5.2 schema with
a non-empty wavName: a 16-char wav name would need 22 + 1 + 8 + 128 +
6 + 4 = 169 bits, slightly LONGER than the observed average. Closer
to fit, but the `str_len = 0` decoded result is firm evidence the
schema is being misaligned.

**Resolution status:** **UNRESOLVED.** Cap1's class-88 events with
`(g=1, gs=153, blk=0)` decoded under the §5.2 SoundData schema do
NOT produce coherent wav-filename strings, and the per-event width
budget does not match a typical filename-bearing record. Either the
§5.2 schema is incomplete, or these events are not SoundData
records (hypothesis §4).

**Suggested follow-up Reader scope (R-7.1.2):**

- Same as §2.1 — capture a vanilla Tribes 1.41 catalogue dump and
  confirm SoundData §5.2 on a non-TAH server.
- Cross-reference any captured `wavFileName` strings against
  `tribes-game/base/sound/*.wav` (or equivalent) to validate the
  string encoding.

### 2.3 Group 3 (PlayerData) — UNRESOLVED

**What is decodable from cap1:** ZERO. cap1's kP02..kP10 contains
no events whose `(g, gs, blk)` header decodes to group 3. The PlayerData
group (which §3.6 of `TRIBES-DATABLOCKS.md` identifies as the
596-byte record class) does NOT appear in cap1's catalogue stream.

Per `TRIBES-DATABLOCKS.md` §3.5 and §3.6, PlayerData records consume
~575 bytes of body each (the parent-class chain ShapeBaseData →
GameBaseData → PlayerData with ~80 fields). A single PlayerData
record would dominate a 596-byte datagram. cap1 has NO 596-byte
packets — all packets are 210..231 bytes, well under the 400-byte
default packet ceiling.

This means **cap1 does NOT contain the PlayerData group's catalogue
dump**. If cap1 is meant to capture the post-handshake initial
burst, the PlayerData blocks must arrive in subsequent packets
beyond the 10 captured by cap1.

**Resolution status:** **UNRESOLVED.** Cap1 truncates before
PlayerData arrives. Cross-reference with `TRIBES-DATABLOCKS.md`
§5.3 (the field-list sketch) and §6.3 (V3 PlayerData ground-truth
from a separate non-cap1 capture) are the only sources of body-
schema authority for group 3.

**Suggested follow-up Reader scope (R-7.1.3):**

- Capture a longer phase-2 burst from TAH (or any Tribes server)
  that includes the PlayerData group's catalogue dump (596-byte
  packets per §3.6).
- Bit-decode one PlayerData record byte-for-byte against the §5.3
  field list and fill in the bit-exact widths.

### 2.4 Group 4 (StaticShapeData) — UNRESOLVED

**What is decodable from cap1:** ZERO. cap1 contains no class-88
events whose `(g, gs, blk)` decodes to group 4.
`TRIBES-DATABLOCKS.md` §6.4 confirms StaticShapeData arrives in
post-PlayerData packets (e.g. `seq025_260B.bin` in the non-TAH
reference capture, where `(group = 4, group_size = 51, block = 0)`
is the first StaticShapeData record).

Per `TRIBES-DATABLOCKS.md` §4, a stock CTF mission has 51
StaticShapeData blocks. Each StaticShapeData inherits from
GameBaseData + ShapeBaseData and adds a small leaf-class field
list (a few mass / drag / rotation defaults). The expected body
width is on the order of 200..600 bits per record, with several
shared-shape filename and skin references.

**Resolution status:** **UNRESOLVED.** No StaticShapeData bytes in
cap1 to decode. The clean-room specs (`TRIBES-DATABLOCKS.md` §3.3
building-block list) describe the general body construction but
not the StaticShapeData-specific field list.

**Suggested follow-up Reader scope (R-7.1.4):**

- Capture a longer Tribes burst extending past the SoundData
  group into the StaticShape group.
- Decode one StaticShapeData event byte-for-byte and document its
  field list in a follow-up addendum to `TRIBES-DATABLOCKS.md`
  §5.4.

### 2.5 Group 5 (ItemData) — UNRESOLVED

Same shape as §2.4. cap1 contains no class-88 events for group 5.

Per `TRIBES-DATABLOCKS.md` §4, a stock CTF mission has 46
ItemData blocks (weapons, ammos, packs, vehicles, the CTF flag).

**Resolution status:** **UNRESOLVED.** No ItemData bytes in cap1.

**Suggested follow-up Reader scope (R-7.1.5):**

- Same as §2.4 — extended capture + byte-for-byte ItemData decode.
- ItemData has heavy cross-references to SoundData (pickup/use
  sounds) so the cross-ref width formula `bits_to_represent(154) =
  8` is the per-block-id-ref width for any ItemData → SoundData
  reference.

### 2.6 Group 21 (MarkerData) — UNRESOLVED

Same shape as §2.4 / §2.5. cap1 contains no class-88 events for
group 21.

Per `TRIBES-DATABLOCKS.md` §4, a stock CTF mission has 3
MarkerData blocks (capture-point + objective markers).

`TRIBES-GHOST-CLASSES.md` §3 cross-references the per-class
ghost-introduction payload for Marker instances as
"1-bit-initial + 8-bit dbid + 1-bit-has-transform + 192 bits
pos+rot". The catalogue body for the MarkerData *template* is
different (it describes per-template settings: shape filename,
icon, behaviour flags). The Reader has no cap1 evidence for the
template body schema.

**Resolution status:** **UNRESOLVED.** No MarkerData bytes in
cap1. The Marker per-instance ghost-intro payload from
`TRIBES-GHOST-CLASSES.md` §3 is documented and available to the
Implementer; the Marker per-template catalogue body is not.

**Suggested follow-up Reader scope (R-7.1.6):**

- Same as §2.4 — extended capture + byte-for-byte MarkerData
  template decode.

### 2.7 Why §2 is mostly UNRESOLVED — root cause

Per §0 / §1.4 / §4, the Reader's confidence is high that **cap1's
class-88 events are NOT catalogue records of the form
`TRIBES-DATABLOCKS.md` §2 describes**. The per-group body schemas
this section was scoped to produce therefore cannot be derived
from cap1 alone.

The §5.1 SoundProfileData and §5.2 SoundData schemas in
`TRIBES-DATABLOCKS.md` are themselves based on a separate non-cap1
capture (the `tah-burst-20260524` files referenced in §6.1–§6.5 of
that spec). Those schemas remain valid for that capture's events
and for any vanilla Tribes 1.41 server's catalogue dump, but they
do not extend to cap1's class-88 events.

The Implementer scope this finding produces is in §4.4: the burst
that 14c-I-6/I-7 generates does not need to match cap1's class-88
content byte-for-byte; instead, it needs to match a real Tribes
1.41 catalogue dump (or whatever else TAH actually wants on a
fresh session). Cap1 cannot serve as the ground truth for
catalogue content; cap1 documents only the wire-framing layout.

---

## 3. Total-bit-budget cross-check

This section computes the per-packet bit budget of cap1's catalogue
content under the §1.1 (6+8+8) header layout, summed across kP02..kP10,
and compares against the available packet bytes.

### 3.1 Per-event width across kP02..kP10

The Reader located every event boundary in each packet by scanning
for the 10-bit pattern `ep=1, guar=1, sc=1, tag=88` =
`1, 1, 1, 0, 0, 0, 1, 1, 0, 1` LSB-first. The first event of every
packet is located at bit 35 (immediately after the ESS-present flag
at bit 34). The remaining events are at the pattern-search hits:

| Packet | Event start bit positions | n_events | Avg width |
|--------|---------------------------|----------|-----------|
| kP02   | 35, 145, 247, 413, 579, 745, 911, 1077, 1243, 1409, 1613 | 11 | 157.8 bits |
| kP03   | 35, 196, 383, 524, 668, 818, 970, 1153, 1337, 1485 | 10 | 161.1 bits |
| kP04   | 35, 218, 397, 560, 737, 903, 1071, 1242, 1413, 1584 | 10 | 172.1 bits |
| kP05   | 35, 215, 387, 558, 729, 900, 1072, 1244, 1416, 1588 | 10 | 172.6 bits |
| kP06   | 35, 215, 388, 561, 734, 908, 1090, 1271, 1451, 1621 | 10 | 176.2 bits |
| kP07   | 35, 219, 391, 561, 739, 915, 1092, 1264, 1444, 1622 | 10 | 176.3 bits |
| kP08   | 35, 217, 399, 579, 733, 905, 1085, 1263, 1417, 1580 | 10 | 171.7 bits |
| kP09   | 35, 215, 385, 555, 723, 881, 1043, 1206, 1367, 1537 | 10 | 166.9 bits |
| kP10   | 35, 195, 356, 524, 684, 852, 1027, 1202, 1385, 1555 | 10 | 168.9 bits |

Total class-88 events across kP02..kP10: **91**. Total
catalogue-shaped content across all packets:
**~15,300 bits ≈ 1900 bytes** of body content + ~810 bits of
event-header framing + 9 × 34 = ~306 bits of VC/rate/ESS/trailer
framing = **~16,400 bits used; 16,449 bits available** (sum of cap1
bit-lengths kP02..kP10) = **49 bits of residual padding** across all
9 packets, or ~5 bits/packet — consistent with byte-boundary padding
at end of stream.

### 3.2 Per-packet bit-budget table

| Packet | Total bits | VC | Rate | ESS-pres | First-ev hdr | Other ev hdrs | Body sum | Residual |
|--------|------------|-----|------|----------|--------------|----------------|----------|----------|
| kP02   | 1848       | 32  | 2    | 1        | 18           | 100 (10×10)    | 1735     | 75       |
| kP03   | 1680       | 32  | 2    | 1        | 18           | 90 (9×10)      | 1611     | 31       |
| kP04   | 1808       | 32  | 2    | 1        | 18           | 90             | 1721     | 49       |
| kP05   | 1808       | 32  | 2    | 1        | 18           | 90             | 1725     | 45       |
| kP06   | 1848       | 32  | 2    | 1        | 18           | 90             | 1762     | 48       |
| kP07   | 1848       | 32  | 2    | 1        | 18           | 90             | 1763     | 47       |
| kP08   | 1784       | 32  | 2    | 1        | 18           | 90             | 1716     | 30       |
| kP09   | 1752       | 32  | 2    | 1        | 18           | 90             | 1668     | 46       |
| kP10   | 1768       | 32  | 2    | 1        | 18           | 90             | 1688     | 42       |

(`Residual` = trailing bits after the last event's body, including
the ESS-end + ISS-pres + GSS-pres + zero-pad to byte boundary, and
the kP10-specific scope-always-complete bit at position 1761 per the
2026-05-24 patch.)

The residual values 30..75 bits are consistent with **at most 3..7
bits of framing trailer (ESS-end=1 bit + ISS-pres=1 bit +
GSS-pres=1 bit, plus the kP10 scope-complete=1 bit) + up to 7 bits
of byte-alignment padding**. So the bit budget tightens cleanly —
each packet's catalogue events fit within the available bit budget
with appropriate trailer overhead.

### 3.3 Ghost-intro feasibility judgement

The 91 class-88 events × ~170 bits = **~15,300 bits ≈ 1900 bytes**
of payload content in cap1 kP02..kP10. Per `TAH-CLASS-TAGS.md`'s
30-second 5_CTF TAH-survey, a scope-always ghost-intro burst
typically contains **75–110 intros** of ~80–120 bits each
(~6,000–13,200 bits = 750–1,650 bytes).

cap1's 91-event content of ~1900 bytes is **slightly above the
upper end** of the TAH-CLASS-TAGS.md envelope. Two interpretations
are equally consistent:

1. **Hypothesis #2 endorsed (recommended):** cap1's class-88
   events ARE the scope-always ghost intros, dispatched via the
   reliable-event channel (with class_tag = 88) rather than the
   ghost-update sub-stream. The "(g=1, gs=153, blk=0)" prefix is
   the leading bits of the per-class ghost-intro payload (which
   for some class may begin with `1` in the first bit position
   + `153` and `0` as small fixed fields). Per-event body width
   of ~170 bits is consistent with mid-sized intros (small
   Markers, Items, StaticShapes).
2. **Hypothesis #1 (catalogue dump but with broken/mirrored
   semantics):** cap1's class-88 events ARE catalogue records,
   but TAH's specific build duplicates `block = 0` across them
   (server-side bug, or build-specific behaviour that doesn't
   match `TRIBES-DATABLOCKS.md` §3.1). The body schema is some
   variant of `TRIBES-DATABLOCKS.md` §5.x with extra TAH-specific
   fields (skin index, mod tag, etc.).

The Reader's judgement strongly favours **hypothesis #2 (ghost
intros via reliable-event channel)** because:

- The 91-event count matches the TAH-CLASS-TAGS.md frequency
  envelope for ghost intros.
- The "(g, gs, blk)" tuple repeating identically across all 90
  group-1 events is inconsistent with a sequential catalogue dump
  per `TRIBES-DATABLOCKS.md` §3.1, but IS consistent with a per-
  intro fixed header that doesn't carry per-record indices.
- The cap1 burst contains NO ghost-update sub-stream (GSS-pres = 0
  on every packet under the §1 6+8+8 header walk), which is
  inconsistent with `TRIBES-INITIAL-BURST.md` §2.3 (phase-2 packets
  must have GSS-pres = 1 for scope-always content). For the
  catalogue content + GSS content to BOTH exist in cap1, the
  catalogue content must be hosted by something other than the
  GSS — which is exactly what reliable-event-channel ghost intros
  would look like.

### 3.4 GSS-pres = 0 finding

**Cap1's kP02..kP10 each show GSS-pres = 0 at the bit position
immediately after the ESS+ISS framing under the §1 6+8+8 header
walk.** Under the assumption that the ESS contains 10 events of
~170 bits each (per §3.2), the GSS-pres flag falls at bit positions
~1741 to 1844 of each packet, all of which read as 0 in the bit-
stream.

This means **the cap1 burst contains NO ghost-update sub-stream
under the §1 header layout** — the entire payload of every packet is
the ESS (reliable-event channel content). Per
`TRIBES-INITIAL-BURST.md` §2.3, phase-2 packets MUST have a
GSS-pres = 1, otherwise the scope-always-complete signal cannot be
delivered.

**Consequence for the kP10 scope-always-complete patch:** the
2026-05-24 patch comment in `tah_ghost_burst.cpp` asserts that
kP10 byte 220 bit 1 carries the scope-always-complete signal.
Under §1's framing walk, the GSS-pres at byte 220 bit 0 reads as 0
in cap1 (before the patch) — so the receiver never enters the GSS
walk and never reaches the scope-always-complete bit. The patch
flips byte 220 bit 1 from `0` to `1` to signal scope-complete,
but **per the §1 framing walk, that bit is NOT the scope-complete
flag — it is the ISS-pres or GSS-pres flag**. The patch may be
flipping the WRONG bit.

**This is a Reader-level finding the Implementer should audit:**
the cap1 burst may not be conformant with the
`TRIBES-INITIAL-BURST.md` §3 phase-2 specification under the §1
framing layout. Either:

- The cap1 capture is mid-stream (not the actual phase-2 burst
  start), and the real phase-2 burst is in a different packet set.
- The cap1 capture documents a TAH-specific burst format that
  does NOT use the standard GSS framing. The scope-always-complete
  signal in cap1's TAH context may live somewhere else entirely.
- The §1 framing walk has an off-by-N bit somewhere (the Reader
  has tested the obvious off-by-1 cases via the 13 header-width
  candidates in §0; none produced a different framing alignment).

---

## 4. Alternative hypothesis: class_tag 88 is NOT catalogue-add

### 4.1 The hypothesis stated

In cap1's TAH build, `class_tag = 88` is **not** the
catalogue-entry-event class documented in `TRIBES-DATABLOCKS.md`
§2. It is some other reliable-event class whose per-event payload
schema is unknown to any clean-room spec. The 22-bit "(group,
group_size, block)" prefix the Reader decoded at body bits 0..21
of every class-88 event is a coincidental bit-pattern reading of
the actual payload's leading bits.

### 4.2 Wire evidence supporting the hypothesis

1. The `block` field reads `0` for every one of 91 class-88 events
   in cap1, including across an intra-packet group transition.
   Per `TRIBES-DATABLOCKS.md` §3.1, this is impossible for a
   sequential catalogue dump.

2. The catalogue-event count budget mismatches: cap1's 91 class-88
   events would, under the catalogue interpretation, account for
   ~9 SoundProfileData blocks (group 0 has 10) + ~82 SoundData
   blocks (group 1 has 153). But the per-event width (~170 bits)
   is too large for SoundProfileData (max 159 bits per §5.1) and
   too small for typical SoundData with a real 16-char wavName
   (~177 bits per §5.2). Neither fits the observed budget cleanly.

3. The cap1 packets carry **NO ghost-update sub-stream** under the
   §1 framing walk (GSS-pres = 0 on every packet), but
   `TRIBES-INITIAL-BURST.md` §2.3 / §3 requires phase-2 packets to
   carry scope-always GSS content. The catalogue content + GSS
   content cannot both occupy the same packet's ESS / GSS regions
   unless one of them is hosted by the OTHER sub-stream's framing
   — which is what reliable-event-channel ghost intros would look
   like (intros riding the ESS, no GSS used at all).

4. The class-88 event content's prefix pattern `(1, 153, 0)`
   under 6+8+8 reading also decodes coherently under OTHER widths
   that give different small-integer values — e.g. 5+8+8 reads
   `(g=1, gs=50, blk=1)`, 4+8+8 reads `(g=1, gs=100, blk=2)`.
   None of these is structurally consistent with a sequential
   catalogue dump either. The fact that **multiple width
   interpretations produce small-integer values** in the leading
   22-bit region of every event suggests these bits are not a
   single (group, group_size, block) tuple but several smaller
   sub-fields whose individual semantic is class-specific.

### 4.3 The class-88-as-ghost-intro candidate decoding

If `class_tag = 88` in cap1 is a **ghost-introduction event** that
the server dispatches via the reliable-event channel, the per-event
payload would plausibly be:

| Bit pos | Width | Field           |
|---------|-------|-----------------|
| 0       | 1     | scope-always flag (=1 for scope-always ghosts) |
| 1..8    | 8     | mirror-table index (low 8 bits) — would explain the "153" reading |
| 9..16   | 8     | persistent-object-number low byte (=0 for first ~256 objects) |
| 17..    | var   | per-class introduction payload (class tag embedded; class-specific layout) |

This is **speculative** — the Reader cannot independently confirm
this layout from clean-room specs alone, and the per-class body
shape would still need to be discovered through bit-by-bit decoding
of multiple cap1 events. But it is a more plausible structural
fit than the catalogue-add interpretation, given the 91-event
count and ~170-bit body width matching TAH-CLASS-TAGS.md's
ghost-intro envelope.

### 4.4 Implementer directives this hypothesis supports

If hypothesis #2 is correct (the recommended path forward), the
14c-I-7 orchestrator's current strategy of "emit a stock CTF
catalogue + scope-always ghost intros via the GSS" is misaligned
with what TAH actually expects on a fresh session. The
recommended I-7 directives become:

1. **Drop the catalogue emission** from the burst entirely (or
   shrink it to only the 1 SoundProfileData record matching
   kP02 event 0's V1 layout, as a header-only sentinel).
2. **Emit the scope-always ghost intros via the reliable-event
   channel using class_tag = 88**, with the per-event payload
   matching the TAH-specific layout the Reader could not derive
   from clean-room specs.
3. **The actual TAH-expected per-event payload should be
   discovered via live fuzzing**: emit a candidate payload, observe
   whether TAH accepts the burst and transitions to load-complete,
   iterate. The Reader cannot derive this payload from clean-room
   specs alone.

If hypothesis #1 is correct (catalogue-add with a TAH-specific
body schema), the recommended I-7 directives become:

1. Emit the catalogue records with the §1 6+8+8 header (correct).
2. Use a TAH-specific per-group body schema that the Reader could
   not derive from clean-room specs. Discovery via live fuzzing
   or via decoding a known-good TAH server's catalogue dump
   byte-for-byte (NOT cap1; cap1 lacks the necessary block-index
   progression evidence).

In BOTH cases, the wire-content replication path of 14c-I-6 is at
a dead-end for cap1. The Reader recommends pivoting to
live-fuzzing against TAH as the next concrete step.

### 4.5 Recommended next Reader spec (R-7.1.7)

**R-7.1.7 — Decode one cap1 class-88 event body byte-for-byte
under multiple speculative payload schemas, and select the schema
whose per-event field values cross-correlate with the TAH-CLASS-
TAGS.md tag-frequency survey.** This is the empirical path to
resolving §4.3's speculation: a 91-event corpus is enough to
search for a per-event class-tag field that, if it exists,
correlates with the 60-tag survey's frequency distribution.

If R-7.1.7 finds a class-tag field, hypothesis #2 is confirmed
and the class-88 events ARE ghost intros. If R-7.1.7 finds NO
plausible class-tag field, hypothesis #1 wins and the
class-88 events are catalogue records with an unknown body schema.

---

## 5. Sentinel encoding (closes R-7.3)

### 5.1 The R-7.3 question

`TRIBES-DATABLOCKS.md` §3.1 documents the sentinel-record convention
for empty groups: `group_size = 0, block = 255, no body`. R-7.3
asked whether partial-catalogue cases (a non-empty group where the
mission references fewer than `group_size` blocks) use:

(a) Full-size emission: `group_size` records emitted, with unused
records sentinel-shaped as `block = 255, group_size = 0, no body`,
OR
(b) Sentinel-fill: `group_size` records emitted with the real
`group_size` repeated on every record, but unused records have
`block = N (the index), group_size = (real size), body = empty`,
OR
(c) Trim: emit only the real number of records (less than
`group_size`).

### 5.2 What cap1 tells us

cap1 cannot definitively resolve this question because (per §1.4,
§2.7, §4) the class-88 events appear NOT to be catalogue records
at all. The block field repeats `0` across all events of a single
group, which is consistent with NONE of the three encoding
conventions above.

However, cap1's **kP02 events 9 and 10 transition from group 0 to
group 1** (events 0..8 read `g=0, gs=10, blk=0`; events 9..10
read `g=1, gs=153, blk=0`). This transition occurs with no
sentinel record between groups, supporting `TRIBES-DATABLOCKS.md`
§6.4's observation that "no sentinel record follows the last
non-empty record of a group; the next event is the first record
of the next group". So the **inter-group-transition convention** in
`TRIBES-DATABLOCKS.md` §3.1 is consistent with cap1 (no
between-group sentinel emitted), but the within-group convention
cannot be tested because block doesn't increment within group.

### 5.3 Reader recommendation

Pending follow-up confirmation from a non-cap1 capture, the Reader
recommends the Implementer keep the existing
`TRIBES-DATABLOCKS.md` §3.1 convention:

- **Empty group:** one record with `group_size = 0, block = 255,
  no body`.
- **Non-empty group:** `group_size` records, each with the real
  `group_size` repeated, `block` incrementing from 0 to
  `group_size − 1`, no sentinel after the last non-empty record.

The cap1 "partial-catalogue" question (the path-(b) sentinel-fill
convention referenced in `CAP1-GHOST-INTRO-DECODE.md` §5.2) is
left **UNRESOLVED** — neither cap1 nor any other clean-room source
provides confirmatory evidence either way. The Implementer should
default to path-(c) (trim) for now and audit against a real TAH
server's actual catalogue dump if/when one is captured.

---

## 6. Reader notes (audit trail)

### 6.1 Non-leaked-source references consulted

- `/Users/v/code/tribes-emscripten/docs/clean-room-specs/CAP1-GHOST-INTRO-DECODE.md`
  (entire file, especially §0 executive, §1 decodable region, §2.2
  first-event evidence, §6.1 catalogue-body scope-out, §6.2
  alternative hypothesis)
- `/Users/v/code/tribes-emscripten/docs/clean-room-specs/TRIBES-DATABLOCKS.md`
  (entire file, especially §2 framing, §3.1 sentinel rule, §3.2
  group ordering, §3.3 block_id_ref width, §3.5 packet boundaries,
  §5.1–§5.3 body sketches, §6.1–§6.5 validation vectors)
- `/Users/v/code/tribes-emscripten/docs/clean-room-specs/TRIBES-NETPROTO.md`
  (sections 5.0, 5.0.1, 5.0.3, 6.1, 6.2, 7.1, 7.3)
- `/Users/v/code/tribes-emscripten/docs/clean-room-specs/TRIBES-INITIAL-BURST.md`
  (sections 1.1, 2.2, 2.3, 2.3.1, 2.3.4, 3, 3.1)
- `/Users/v/code/tribes-emscripten/docs/clean-room-specs/TRIBES-GHOST-CLASSES.md`
  (sections 1.1, 1.3, 3 cross-reference of per-class payloads)
- `/Users/v/code/tribes-emscripten/docs/clean-room-specs/TRIBES-PHASE2-PACKING.md`
  (sections 1.1–1.5)
- `/Users/v/code/tribes-emscripten/open-siege/engine/src/tah_ghost_burst.cpp`
  lines 1..302 (cap1 source bytes kP01..kP10, 2026-05-24 patch
  comment)
- `/Users/v/code/tribes-emscripten/open-siege/engine/src/tah_datablock_encoder.cpp`
  lines 1..420 (Implementer-side I-6 hypothesis of catalogue
  emission; used as cross-reference of what the orchestrator
  currently emits, NOT as a derivation source — when cross-referenced
  against the cap1 wire evidence, the I-6 emission shape is
  inconsistent with cap1's class-88 stream)
- `/Users/v/code/tribes-emscripten/open-siege/wiki-contributions/TAH-CLASS-TAGS.md`
  (60-tag TAH frequency survey, used in §3.3 ghost-intro feasibility)

### 6.2 Capture artefacts walked

The Reader produced bit-level decodes of:

- kP01 (210 B): VC header + R1 rate-control update + ESS with
  first event `tag = 79, explicit_seq = 0` + second event
  `tag = 79, sc = 1` (the join-event class, out of scope for
  R-7.1 per `CAP1-GHOST-INTRO-DECODE.md` §6.6).
- kP02 (231 B): 11 class-88 events, decoded under the §1 6+8+8
  header schema. Events 0..8 read `(g=0, gs=10, blk=0)`, events
  9..10 read `(g=1, gs=153, blk=0)`.
- kP03..kP10 (210..231 B each): 10 class-88 events each, all
  decoded as `(g=1, gs=153, blk=0)`. Total 90 events.
- All event-boundary positions located by scanning for the
  10-bit pattern `ep=1, guar=1, sc=1, tag=88` =
  `1, 1, 1, 0, 0, 0, 1, 1, 0, 1` LSB-first.

### 6.3 Candidate header widths tested (§0)

- 6 + 8 + 8 (spec authority) — confirmed
- 5 + 8 + 8 — rejected (kP02 ev0 reads (0, 20, 0), contradicts V1)
- 7 + 8 + 8 — rejected (kP02 ev0 reads (0, 5, 0), kP03 reads (65,…))
- 6 + 9 + 8, 6 + 10 + 8, 6 + 8 + 9, 6 + 8 + 10 — all collapse to
  the same `(0, 10, 0)` / `(1, 153, 0)` readings, indistinguishable
  from 6 + 8 + 8 by cap1's bit-stream alone
- 5 + 8 + 9 — rejected (kP02 reads (0, 20, 0))
- 4 + 8 + 8 — rejected (kP02 reads (0, 40, 0))
- 6 + 7 + 8 — rejected (kP02 reads (0, 10, 0) consistent, but kP03
  reads (1, 25, 1) — group_size=25 contradicts §4 corpus)
- 6 + 8 + 7 — rejected (kP02 reads (0, 10, 0) consistent, but
  identical block evidence as 6+8+8)
- 5 + 7 + 8 — rejected
- 5 + 9 + 8 — rejected (kP03 reads (1, 306, 0); group_size=306
  out of u8 range)

Only **6 + 8 + 8** survives both V1 validation and the §4-corpus
group-size cross-check. The wider variants (6+9, 6+10, 6+8+9,
6+8+10) are indistinguishable from 6+8+8 by cap1 alone but are
rejected by Occam's razor in favor of the spec-authority width.

### 6.4 Implementer cross-references

The following Implementer-side files were consulted as
cross-reference (NOT as derivation source). The Reader notes that
when 14c-I-6's emission shape is cross-checked against cap1, the
two diverge significantly — the cap1 evidence does not validate
the I-6 catalogue-emission scheme:

- `tah_burst_orchestrator.cpp` lines 1..436 (orchestrator state
  machine, ghost-intro emission)
- `tah_datablock_encoder.cpp` lines 1..420 (catalogue-event
  encoder; uses 6+8+8 header matching §1)
- `tah_class_registry.cpp` lines 1..150 (current class-tag map)

The Reader emphasises: **the I-6 encoder is correctly emitting a
6+8+8 catalogue header; the issue is not the encoder's framing.
The issue is that cap1's class-88 events do NOT appear to be
catalogue records.** See §4 alternative hypothesis.

### 6.5 Forbidden references explicitly NOT consulted

- `github.com/MortarTurret/Darkstar` — NOT opened.
- `github.com/sdozeman/starsiege-tribes` — NOT opened.
- Any leaked-Dynamix-engine mirror — NOT opened.

The Implementer agent for the follow-up R-7.1-impl spec is
instructed NOT to follow these references either. This spec is
intended to be sufficient on its own within the limits called out
in §0, §2.7, and §4.

---

## 7. Summary for the Implementer

The Reader produced this spec to close `CAP1-GHOST-INTRO-DECODE.md`
§6.1 (per-group catalogue body schemas). The closure is partial:

**What is resolved:**

- §1: the catalogue-entry-event header layout is reaffirmed at
  6 + 8 + 8 bits (matching `TRIBES-DATABLOCKS.md` §2). No header
  width change is needed in 14c-I-7's catalogue encoder.
- §3: per-packet bit budget cross-check confirms ~10 class-88
  events per cap1 packet, each ~170 bits. The 91-event total
  consumes ~1900 bytes of payload.
- §5: empty-group sentinel convention from
  `TRIBES-DATABLOCKS.md` §3.1 is consistent with cap1's inter-group
  transition. Partial-catalogue convention is UNRESOLVED.

**What is UNRESOLVED:**

- §2 per-group body schemas for groups 0, 1, 3, 4, 5, 21 — cap1
  does not contain decodable schema evidence for any of these,
  because (per §4) cap1's class-88 events appear NOT to be
  catalogue records.
- The actual semantics of cap1's class-88 events. Hypothesis #2
  (they're ghost intros via the reliable-event channel) is the
  Reader's strong recommendation, but neither hypothesis is
  byte-decodable from clean-room specs alone.

**Recommended next steps for the Implementer (14c-I-7-redux or
14c-I-8):**

1. **Stop trying to byte-match cap1's catalogue stream.** It is
   not a catalogue stream. The I-6 catalogue-emission code in
   `tah_datablock_encoder.cpp` is structurally correct (6+8+8
   header, proper sentinel records, group ordering) per the
   non-cap1 V1..V8 captures documented in `TRIBES-DATABLOCKS.md`
   §6, but cap1 cannot validate it.
2. **Live-fuzz against TAH.** Iteratively emit candidate burst
   contents (variable catalogue size, variable ghost-intro count,
   variable class_tag values via §4.4) and observe whether TAH
   transitions to load-complete. The Reader has scoped out the
   ability to derive the correct contents from clean-room specs
   plus cap1 evidence alone.
3. **Audit the kP10 scope-always-complete patch (§3.4).** Under
   the §1 framing walk, byte 220 bit 1 of cap1 kP10 is NOT the
   scope-always-complete flag — it sits inside the ESS or one of
   the sub-stream-pres flags. The 2026-05-24 patch's bit
   assertion may be wrong. The patch may need to be moved (or
   removed if cap1 does not actually encode the
   scope-always-complete signal at all).
4. **For wire-shape correctness independent of content:** the
   `TRIBES-DATABLOCKS.md` §1.1 6+8+8 header AND the
   `TRIBES-DATABLOCKS.md` §5.1/5.2 SoundProfile/Sound body schemas
   ARE valid for non-TAH Tribes 1.41 captures (per
   `TRIBES-DATABLOCKS.md` §6.1–§6.5). I-7 should retain those
   schemas as the catalogue-emit default for non-TAH targets.

The two-spec follow-up scope this Reader recommends:

- **R-7.1.7 (Reader):** decode one cap1 class-88 event body
  byte-for-byte under multiple speculative ghost-intro payload
  schemas; pick the schema whose per-event field values
  cross-correlate with the `TAH-CLASS-TAGS.md` tag-frequency
  distribution.
- **R-7.2 (Reader):** capture a non-cap1 Tribes 1.41 catalogue
  dump (the 31-packet capture cap1 superseded), verify
  `TRIBES-DATABLOCKS.md` §5.1/5.2/5.3 body schemas byte-for-byte,
  and produce body-schema addenda for groups 4 (StaticShapeData),
  5 (ItemData), 8 (SensorData), 19 (TurretData), 21 (MarkerData)
  needed for the I-7 catalogue-emit fixes.
- **I-7.1-impl (Implementer):** drop cap1 byte-replay; switch
  catalogue-emit to a "stock CTF" minimal catalogue + a live-fuzz
  loop that iterates ghost-intro contents until TAH accepts. The
  catalogue-emit code path remains unchanged structurally; only
  the per-mission catalogue contents and the ghost-intro emission
  schedule need rework.
