# Tribes 1 / TAH wire protocol — comprehensive clean-room spec

**Status:** v1 (2026-06-16). This spec consolidates and supersedes the
"Phase 1 / Phase 2 content stream" gaps that
`TRIBES-PROTOCOL-PCAP-DIFF.md`, `CAP1-GHOST-INTRO-DECODE.md`, and
`TRIBES-CATALOGUE-BODIES.md` documented as UNRESOLVED. It is intended
to be the single spec an Implementer needs in order to bring our
open-siege t1-server's TAH session from "stalled at handshake" to
"reaches the load screen". It does NOT replace `TRIBES-NETPROTO.md`
for the VC framing primitives — it references that spec for sections
that are already correct and only restates where wire evidence has
updated knowledge.

**Provenance:**

- **Frohnmayer + Gift, "The TRIBES Engine Networking Model"** (GDC
  2000 paper) — public Dynamix design document. Cited as
  `paper §<section>`. Quoted and paraphrased freely; this is a
  published article.
- **Netcode-rev20 corpus** under
  `docs/clean-room-specs/netcode-rev20/`. Community-written clean-room
  Tribes 1 client (verified provenance per
  `netcode-rev20/PROVENANCE.md`). Cited as `rev20/<file>:<function>`.
  Text references (`t1events.txt`, `t1demoformat.txt`,
  `t1eventsother.txt`, `t1structure.txt`) describe protocol facts and
  are quoted in moderation. Source files are paraphrased: structure
  + behaviour described in this Reader's own words, no verbatim
  function-body transcription.
- **Live wire capture:** `tribes-game/captures/tah-to-public-2026-06-16.pcap`
  — 4905-packet, ~13-minute session of TAH against
  `104.128.49.65:28006` (public TAH server). Used as ground truth on
  all disagreements between docs.
- **Pre-existing clean-room specs** (`TRIBES-NETPROTO.md`,
  `TRIBES-DATABLOCKS.md`, `TRIBES-PROTOCOL-PCAP-DIFF.md`,
  `TAH-REQUESTCONNECT-SHAPES.md`, etc.) — listed in Appendix A.

**Forbidden references explicitly NOT consulted:**
`github.com/MortarTurret/Darkstar`, `github.com/MortarTurret/extinction`,
`github.com/sdozeman/starsiege-tribes`,
`github.com/BaseEncrypt/TotalAnnihilation`, `github.com/kingtomato/tribes.tools`,
and any other leaked-Dynamix-engine mirror.

---

## §0 — Executive summary

The protocol model from the Frohnmayer paper, the per-event byte
schemas from `netcode-rev20/t1events.txt`, the BitStream conventions
from `netcode-rev20/bitStream.{cpp,h}`, and the live wire bytes in
`tah-to-public-2026-06-16.pcap` mutually confirm a single coherent
description of the Tribes 1 wire protocol. This spec closes nearly
every UNRESOLVED item the prior pcap-diff Reader flagged:

- **All event class tags observed in the public pcap are now named**
  (§2). The previously-mysterious wire tags 8, 75, 77, 79, 88 decode
  to SimConsoleEvent, PlayerAddEvent, DeltaScoreEvent, TeamAddEvent,
  and DataBlockEvent respectively. Tag 112 was a bit-position
  misread (PingPLEvent tag=97 mis-decoded).
- **TAH ClientReady is two SimConsoleEvent (remoteEval) messages** —
  not opaque (§7.4). The first is `zAdminActiveMode` (admin probe);
  the second is `SetCLInfo("Entities/base", "", "", "", "", "", "2",
  "false", "-1")` — 10-arg client-info publish.
- **The 6+8+8 catalogue header is verified** (§4) and the per-group
  body schemas are enumerated for every datablock type the
  Blastside session uses (§4.4..§4.30).
- **Steady-state OSGB content is one of three packet shapes** (§3):
  ESS-only (reliable events), GSS-only (per-tick ghost deltas, ~59 B,
  ~15 Hz), or PSC-only (Move-Manager control-object state, ~10–40 B).
  Our current 28 B near-empty packets fail TAH's parser because they
  carry no valid substream payload past the VC header.

The remaining UNRESOLVED items are narrow: (a) the per-class ghost
intro / update encoder for the GSS, which the rev20 corpus does NOT
implement (it skips both PlayerPSC and GhostManager); (b) the exact
opaque byte-payloads of the StaticData / VehicleData / etc.
"burn data" bit-runs (rev20 reads these as opaque bit-skips); (c)
the `scope-always-complete` 1-bit signal, which sits inside the GSS
trailer and therefore inherits (a)'s blocker. §9 lists these
explicitly with evidence pointers.

---

## §1 — Architecture model (from the paper)

### §1.1 — Data delivery classifications

Per `paper §Overview`, every byte transmitted falls into one of four
classifications. The classification determines which Stream Manager
owns the data and which retransmit/ordering guarantees apply.

| Class | Name                  | Semantics                                          | Managed by |
|-------|------------------------|----------------------------------------------------|------------|
| 1     | Non-guaranteed         | Never re-transmitted if lost                       | Event mgr (subset), Ghost mgr |
| 2     | Guaranteed             | Re-transmitted until delivered; ordered           | Event mgr, Datablock mgr |
| 3     | Most-Recent-State      | Only the latest value matters; lost updates replaced by newer ones | Ghost mgr, Move mgr (control-object state) |
| 4     | Guaranteed Quickest    | Sent in every packet until acknowledged; no ordering | Move mgr (input moves) |

Classification 3 is implemented as a per-object dirty-bit mask. When a
packet carrying a dirty bit is acked, the bit is cleared; when it is
dropped, the bit is set again — UNLESS a subsequently-sent packet
already carried the same bit set, in which case the loss is ignored
(`paper §Ghost Stream Manager`). This is the "stale data" optimisation
that makes the protocol cheap.

### §1.2 — Three-layer model

Per `paper §Overview`:

```
+------------------------------------------------+
| Simulation Layer                              |
|   (game objects, scoping, client prediction)  |
+------------------------------------------------+
| Stream Layer                                  |
|   Stream Manager                              |
|     -> Event Manager  (reliable + guaranteed) |
|     -> Ghost Manager  (most-recent-state)     |
|     -> Move Manager   (guaranteed quickest)   |
|     -> Datablock Mgr  (asymm. via Event evts) |
|     -> String Mgr     (Huffman compression)   |
+------------------------------------------------+
| Connection Layer                              |
|   Connection Manager                          |
|     -> Platform Packet Module (UDP socket)    |
+------------------------------------------------+
```

The Connection Layer guarantees only **notification** — every sent
packet eventually produces either a "delivered" or "dropped" event.
Re-transmission is the Stream Manager's responsibility. This is the
key insight that lets all four classifications coexist on the same
UDP socket without redundant ack machinery.

### §1.3 — Five stream managers

Per `paper §Stream Layer` and corroborated by
`rev20/t1demoformat.txt` (the demo packet processing pseudocode):

| Manager | Wire-level role | What it writes |
|---------|-----------------|----------------|
| **Stream** | Allocates the packet, runs rate-control, emits substream-present flags. | 2-bit rate-control prefix, 3-bit substream presence (ESS / PSC / GSS). |
| **Event** | Reliable / guaranteed events. ChatSay, scoreboard, datablocks, remoteEvals, etc. | Event SubStream (ESS): a stream of length-tagged events ending in a 0-bit terminator. |
| **Move (PSC)** | Client→server: 32 ms input moves (forward/back/strafe + yaw/pitch + triggers). Server→client: Control-Object state (player avatar position/velocity/orientation). | Per-Substream Control (PSC): variable-width payload, contents class-specific. |
| **Ghost** | Per-tick state diffs for scoped game objects. | Ghost SubStream (GSS): variable-width sequence of (ghost-id, status, mask, body). |
| **Datablock** | Catalogue ("static reference data") delivered as guaranteed events. Asymmetric: server→client only. | NOT a separate substream — emits `DataBlockEvent`s (class 0x458) through the Event Manager. |
| **String** | Huffman compression of strings inside event/datablock bodies. | A per-string `1-bit compressed + 8-bit length + body` (see §1.5). |

`rev20/T1NetStream.cpp:ReadPacket` only routes to the EventManager;
the rev20 author explicitly skips PlayerPSC and GhostManager
(`packet.writeFlag(false); // playerpsc \n packet.writeFlag(false); // ghostmanager`).
The Implementer SHOULD parse all three substreams server-side; the
content of PSC/GSS is described in §5/§3 below.

### §1.4 — Sliding-window connection manager

Per `paper §Connection Layer` + `TRIBES-NETPROTO.md` §3.6 + pcap
evidence. The connection layer:

1. Numbers every outgoing packet with a **9-bit send-seq** (mod 512).
2. Carries a **5-bit highest-received-seq** (mod 32) in every packet
   for fast cumulative ack.
3. Carries a **run-length encoded ack list** for any non-contiguous
   receipt. Each list element is `(3-bit count - 1, 5-bit seq)`. The
   list terminates with `count==0` followed by the 5-bit packet type.
4. Maintains a 32-packet outstanding window. When
   `(send_seq - highest_acked_seq) >= 32`, transmission stalls
   pending an ack.
5. Generates **packet-notify events** to the Stream layer for every
   sent packet in send order: either "delivered" (acked) or "dropped"
   (acked past, with this seq missing from the run-list).

Per `paper §Connection Layer`: average overhead is **3 bytes per
packet** for the ack machinery alone. The pcap confirms 4-byte pure
acks for connect-handshake confirmations (pkt 3: `07 08 09 80`).

### §1.5 — BitStream conventions

Source: `rev20/bitStream.{cpp,h}` (paraphrased) +
`paper §Connection Layer` ("bit packing").

- **Byte order within a packet:** LSB-first. Bit 0 of a multi-bit
  field is the lowest bit; bit (N-1) the highest. Verified by every
  successful decode in this spec.
- **Single-bit flag:** read 1 bit, return as a bool. Implemented as
  `*ptr & (1 << (bitpos & 7))` for byte `ptr = data + (bitpos >> 3)`.
- **Variable-length integer:** `read_int(N)` reads N bits. For values
  larger than 8, the byte ordering is little-endian by virtue of the
  LSB-first bit packing (a 32-bit int at byte-aligned bitpos reads as
  `*(uint32_t*)ptr` on a little-endian host).
- **Signed integer:** 1 bit sign (1=negative), then `read_int(N-1)`
  for the magnitude (NOT two's complement). Note this means `-0` is
  representable and zero has two encodings; the encoder typically
  emits sign=0 for zero.
- **Variable-length float:** `read_float(N)` reads N bits as an
  unsigned integer in `[0, 2^N - 1]` and returns
  `value / (2^N - 1)`. Range `[0, 1]`. Used for normalised values
  like volume, pitch.
- **Signed float:** read_int(N), maps `[0, 2^N-1]` linearly to
  `[-1, +1]`. Range bias of half a quantum on signed_float(0).
- **Normal vector:** `read_normal_vector(N)` consumes `2N+1` bits and
  produces a unit vector in 3D. The encoding is not documented in
  rev20 (the function is a stub that just burns bits — see
  `rev20/bitStream.cpp:readNormalVector`). The Implementer can treat
  it as opaque for spec purposes: just burn the bits.
- **Huffman string:** 1-bit "compressed" flag + 8-bit length + body.
  - If compressed=0: body is `length` raw bytes (8 bits each).
  - If compressed=1: body is `length` Huffman-coded symbols using a
    256-entry static-table Huffman tree.

The Huffman tree is built from the 256-element `mCharFreqs` table
present in `rev20/bitStream.cpp:CHAR_FREQS`. This table is a
data constant (raw integer frequencies), not copyrightable
expression. The Implementer SHOULD copy the table verbatim. The
tree-build algorithm is the standard pop-up-two-smallest construction
described in `paper §Stream Layer`:

> "construct the array of leaves... repeatedly merge the two
> lowest-popularity wrappers into a new internal node... continue
> until one wrapper remains."

When the encoder finds that `numBits >= len * 8` (i.e. compression
would expand the string), it falls back to compressed=0. Strings of
length 0 emit `[1-bit flag][8-bit zero]` regardless. The maximum
encoded string length is 255 bytes.

### §1.6 — Per-packet structure

Per `rev20/t1demoformat.txt` + pcap-confirmed bit positions:

```
[bit 0]        VC-present flag (= 0 for OOB discovery, = 1 for VC)
[bit 1]        Connection parity (= LSB of nonce[0])
[bits 2..10]   send_seq (9 bits)
[bits 11..15]  hrcv (5 bits = highest received seq, mod 32)
[bits 16..N]   Ack run list: zero or more (3-bit count, 5-bit seq)
[bits N..N+2]  Ack-list terminator (3 bits = 0)
[bits N+3..N+7] Packet type (5 bits)
[if connect-control type] 32-bit nonce
[if DataPacket]:
   [1 bit]    R0 (current-rate-changed flag)
   [if R0]    [10-bit cur_update_delay][10-bit cur_packet_size]
   [1 bit]    R1 (max-rate-changed flag)
   [if R1]    [10-bit max_update_delay][10-bit max_packet_size]
   [1 bit]    ESS-present flag (Event SubStream)
   [if ESS]   <event substream — see §2>
   [1 bit]    PSC-present flag (Player-State Control / Move Manager)
   [if PSC]   <move-manager substream — see §5>
   [1 bit]    GSS-present flag (Ghost SubStream)
   [if GSS]   <ghost substream — see §3>
[zero-pad to next byte boundary]
```

Important properties:

- **ESS / PSC / GSS flags are in this fixed order.** The pcap
  confirms it: every S→C and C→S DataPacket reads these three flags
  in sequence and acts on each present substream in turn.
- **No length prefix on any substream.** Each substream's terminator
  / end-of-content marker is part of its payload schema. ESS uses a
  trailing 0-bit (no more events); GSS uses a per-ghost iteration
  loop terminator (1-bit "object-present", value 0 ends the loop).
- **Substreams that aren't present consume zero bits**, not even an
  internal length field — the flag IS the entire absence indicator.
- **Padding:** zero bits to next byte boundary at end of payload
  (per `rev20/bitStream.cpp:writeBits`).

### §1.7 — Conflicts with `TRIBES-NETPROTO.md`

The existing `TRIBES-NETPROTO.md` v1 spec was derived before this
corpus and has a few errors:

| `TRIBES-NETPROTO.md` claim | Reality (per this spec) |
|----------------------------|--------------------------|
| Header is MSB-first within each byte | LSB-first within each byte (`rev20/bitStream.cpp:readBits` and pcap-confirmed) |
| `Packet type` is 5 bits at end of ack list | Confirmed — `count==0` followed by 5-bit ptype |
| Substream-presence order is "ESS, ISS, GSS" using the name "ISS" (input substream) | The middle substream is PlayerPSC (Player-State Control / Move-Manager). Functionally same — input moves C→S and control-object state S→C — but the Implementer should use the more accurate name. The `TRIBES-NETPROTO.md` "ISS" is this spec's "PSC". |
| Pkt-type values 0..4, 7, 16 are the wire values | Confirmed (pkt-type 12 = AcceptConnect appears in pkt 2, not 3; see below) |
| AC pkt-type is "3" | Wire evidence: AC pkt 2 byte 3 = `0x60`, which under LSB-first decodes ack-term=0, ptype=12. The 5-bit ptype field at bit 27..31 reads `01100`b = 12. This conflicts with `TRIBES-NETPROTO.md` §3.2 mapping but the wire is authoritative. The numeric value 12 has no `rev20/T1Connection.h:Packet Types` analog, but `kAccepted=3` IS the value in the rev20 enum; the discrepancy is left as-is (likely a packet-type bit-allocation re-numbering in the live build). |
| `windowFull()` at 32 unacked | Confirmed |
| RTT EWMA = `(sample + 31*avg)/32` | Confirmed |

The Implementer should treat THIS spec as authority on bit-orientation
and substream order. `TRIBES-NETPROTO.md` remains correct on transport,
discovery, master-server protocol, and the connection state machine.

---

## §2 — Event Manager events (the post-burst content layer)

### §2.1 — Event SubStream framing

Per `rev20/t1demoformat.txt` (lines under "Process EventManager
Packet") and pcap-confirmed via every successful event-walk in this
spec.

For each event in the substream:

```
[1 bit]    event-present (= 0 ends the substream)
[1 bit]    flag_a
[if flag_a]:
    [1 bit]   flag_b
    [if !flag_b]:
        [1 bit]   flag_c (= has_explicit_seq)
        [if flag_c]:
            [7 bits]  explicit_seq
[7 bits]   wire_class_tag (real class = wire + 0x400)
[var bits] event body — schema per class (see §2.3..§2.21)
```

The three flags (`flag_a`, `flag_b`, `flag_c`) implement the
ordering / sequencing scheme for guaranteed-event delivery (see
`paper §Event Stream Manager`). In practice on the wire:

- **First event of an ESS** typically has `flag_a=1, flag_b=0,
  flag_c=1` → reads a 7-bit explicit sequence number. (Verified: pub
  pkt 5 first event reads `eseq=0`, pkt 6 reads `eseq=6`, pkt 8 reads
  `eseq=35`, etc. — these are the running guaranteed-event sequence
  numbers.)
- **Subsequent events** in the same packet typically have
  `flag_a=1, flag_b=1` → no explicit_seq; the seq increments by 1
  from the previous event in this packet.
- The "non-explicit, non-continuous" case (`flag_a=0`) appears on
  resend-after-loss; the read code reads the 7-bit tag immediately,
  no sequencing.

Once `event-present=0` is read the substream ends. The decoder then
proceeds to read the PSC-present flag (§5).

**Length of an event body is implicit in its class.** The class-tag
selects a per-event decoder which reads exactly the right number of
bits. There is NO length-prefix in the event header.

### §2.2 — Event class catalog

The events that appear in either direction of the Tribes 1 / TAH
wire protocol. Cited from `rev20/t1events.txt`, `t1eventsother.txt`,
and `rev20/T1EventManager.h` (header `#define` enumeration). All tag
values are stated as **wire tags**; the in-source class ID is
`wire_tag + 0x400`.

| Wire | Real    | Name | Direction in pcap | t1events.txt? | Decode below |
|------|---------|------|--------------------|----------------|--------------|
| 0    | 0x400   | SimTimeEvent              | server→client | t1eventsother | §2.3 |
| 1    | 0x401   | SimMessageEvent           | both         | t1eventsother | §2.4 |
| 5    | 0x405   | SimObjectTransformEvent   | server→client | t1eventsother | §2.5 (SimEvent base) |
| 8    | 0x408   | SimConsoleEvent           | both         | t1events.txt  | §2.6 |
| 10   | 0x40a   | SimTimerEvent             | server→client | t1eventsother | (SimEvent base) |
| 32   | 0x420   | SimTriggerEvent           | server→client | t1eventsother | (SimEvent base) |
| 35   | 0x423   | SimSoundSequenceEvent     | server→client | t1eventsother | §2.7 |
| 37   | 0x425   | SimPathEvent              | server→client | t1events.txt  | §2.8 |
| 41   | 0x429   | SimRegisterTextureEvent   | server→client | t1eventsother | (SimEvent base) |
| 75   | 0x44b   | PlayerAddEvent            | server→client | t1events.txt  | §2.9 |
| 76   | 0x44c   | PlayerRemoveEvent         | server→client | t1events.txt  | §2.10 |
| 77   | 0x44d   | DeltaScoreEvent           | server→client | t1events.txt  | §2.11 |
| 79   | 0x44f   | TeamAddEvent              | server→client | t1events.txt  | §2.12 |
| 80   | 0x450   | MissionResetEvent         | server→client | t1events.txt  | §2.13 |
| 82   | 0x452   | PlayerTeamChangeEvent     | server→client | t1events.txt  | §2.14 |
| 83   | 0x453   | PlayerSayEvent            | server→client | t1events.txt  | §2.15 |
| 85   | 0x455   | PlayerCommandEvent        | server→client | t1events.txt  | §2.16 |
| 86   | 0x456   | PlayerSelectCmdrEvent     | server→client | t1events.txt  | §2.17 |
| 88   | 0x458   | DataBlockEvent            | server→client | t1events.txt  | §4 |
| 89   | 0x459   | TeamObjectiveEvent        | server→client | t1events.txt  | §2.18 |
| 92   | 0x45c   | LocSoundEvent             | server→client | t1events.txt  | §2.19 |
| 93   | 0x45d   | TargetNameEvent           | server→client | t1events.txt  | §2.20 |
| 95   | 0x45f   | VoiceEvent                | server→client | t1events.txt  | §2.21 |
| 96   | 0x460   | SoundEvent                | server→client | t1events.txt  | §2.22 |
| 97   | 0x461   | PingPLEvent               | server→client | t1events.txt  | §2.23 |
| 98   | 0x462   | PlayerSkinEvent           | server→client | t1events.txt  | §2.24 |

Several derived "SimEvent" classes (`SimObjectTransformEvent`,
`SimTimerEvent`, `SimTriggerEvent`, `SimRegisterTextureEvent`) read
the same base format. The SimEvent base body, per `t1eventsother.txt`,
is:

```
if (readFlag) { readInt(32) }
else if (readFlag) { readInt(10) }
else { readInt(32) }
```

This is a three-way branch implementing a time-tag with two precisions
(10-bit short tag, 32-bit long tag) plus a fallback. Implementers
should treat SimEvent-base classes as "read 1-2 flags + an integer of
the indicated width" and continue. Most SimEvent subclasses add no
further body.

### §2.3 — SimTimeEvent (0x400)

Inherits `SimEvent` (§2.5). No additional fields. Used by the server
to advertise the current sim-time tick.

### §2.4 — SimMessageEvent (0x401)

```
read SimEvent_base
if (readFlag)        readInt(1)
else if (readFlag)   readInt(4)
else if (readFlag)   readInt(8)
else if (readFlag)   readInt(16)
else if (readFlag)   readInt(32)
```

(Per `t1eventsother.txt`.) Variable-width single integer (1/4/8/16/32
bits) selected by leading 1-bit flags. Used for generic sim-layer
notifications.

### §2.5 — SimEvent base body

```
if (readFlag) { readInt(32) }
else if (readFlag) { readInt(10) }
else { readInt(32) }
```

The 10-bit branch is the short timestamp; both 32-bit branches are
the same long timestamp encoding (no clear functional distinction in
the rev20 docs).

### §2.6 — SimConsoleEvent (0x408) — "remoteEval"

The **most important Event class** for handshake content. This is the
mechanism by which the server pushes script commands to the client
(and the client publishes its CLInfo / dataFinished signal to the
server).

```
[5 bits]  argc       — number of strings including the function name
for i in 0 .. argc-1:
    [huff string]   arg[i]    (arg[0] is the function/command name)
```

**Notes:**

- `rev20/T1EventManager.cpp:SimConsoleEvent_Unpack` reads `argc - 1`
  string arguments after the command, treating the command as a
  separate field. The wire format reads `argc` strings total — the
  Implementer should treat the first as the command and the rest as
  arguments.
- `argc=0` is allowed; rev20 short-circuits and returns. The pcap
  contains no `argc=0` cases.
- argc is 5 bits wide so the maximum is 31 strings per call. The
  largest observed in our pcap is 10 (`SetCLInfo`).

**Pcap evidence (verbatim decoded payloads, all from
`tah-to-public-2026-06-16.pcap`):**

| Direction | When | Command | Args |
|-----------|------|---------|------|
| C→S (ClientReady pkt 4) | t=0.042s | `zAdminActiveMode` | (none) |
| C→S (ClientReady pkt 4) | t=0.042s | `SetCLInfo` | `"Entities/base"`, `""`, `""`, `""`, `""`, `""`, `"2"`, `"false"`, `"-1"` |
| C→S (catalogue done pkt 130) | t=3.310s | `dataFinished` | (none) |
| S→C (pkt 5 phase 1) | t=0.054s | `SVInfo` | `"1.11"`, `"Rosco's Mixtape 1999 - PlayT1.com/discord"`, `"base"`, `"Join the Tribes Discord @ playt1.com/discord - Server Version 1.3.1"`, `""` |
| S→C (pkt 5 phase 1) | t=0.054s | `MODInfo` | `"Server is optimized with the <f2>Pickup<f2> ..."` |
| S→C (pkt 6 first event) | t=0.119s | `FileURL` | `""` |
| S→C (pkt 129) | t=3.302s | `zAdminActiveMode` | `"true"`, `""` × 8 (set ack) |
| S→C (pkt 129) | t=3.302s | `TeamScore` | `"0"`, `"0"`, `""` × 8 |
| S→C (pkt 129) | t=3.302s | `TeamScore` | `"1"`, `"0"`, `""` × 8 |

The handshake protocol is:

1. Server sends AcceptConnect (16 B).
2. Client acks AC.
3. **Client sends ClientReady DataPacket** containing 2 remoteEvals:
   `zAdminActiveMode` (admin probe — returns a 1-bit truth value) and
   `SetCLInfo(entity_pack, password, ...)`.
4. Server sends Phase 1 (TeamAdds, PlayerAdd for the joiner,
   SVInfo / MODInfo SimConsoleEvents).
5. Server sends Phase 2 — `FileURL` remoteEval (for HTTP file-download
   redirection), then all DataBlockEvents (the catalogue).
6. Server emits `zAdminActiveMode("true")` + `TeamScore` SimConsoleEvents to
   finalise the join-banner.
7. Client sends `dataFinished` remoteEval ("I've ingested everything;
   start the per-tick stream").
8. Server transitions into steady-state ghost / move stream.

This is the **content protocol** for unblocking the TAH load screen.

### §2.7 — SimSoundSequenceEvent (0x423)

```
read SimEvent_base
readInt(4)         — burns 4 bits
readInt(6)
readFlag
readInt(52)        — burns 52 bits
```

Per `t1eventsother.txt` (which writes it as `_read(52)` = burn 52
bits). Used for ambient looping sound triggers in mission scripts.

### §2.8 — SimPathEvent (0x425)

```
read SimEvent_base
readInt(32)              — burns 32 bits
if (readFlag):
    readFlag             — direction?
    count = readInt(32)
    loop count times:
        readInt(30)      — burns 30 bits per knot (likely 3×10-bit position)
```

Mission-path waypoint definition; a sequence of 30-bit packed knots.

### §2.9 — PlayerAddEvent (0x44b)

```
player_id        = readInt(32)
                   readFlag           — burned (unknown role)
player_name      = readString          — Huffman, ≤ 256 bytes
player_skin      = readString
player_voice     = readString
                   readInt(1)          — burned
                   readInt(32)         — burned
if (readFlag):
    player_team  = readInt(3)         — team_id in [0..7]
else:
    player_team  = -1                  — unassigned
```

Per `rev20/t1events.txt`. The 32-bit and 1-bit fields between the
voice string and the team flag are uninterpreted in rev20
(`event[c8] = readInt(1); _read(event[8c], 4)`). Implementer should
emit zero for those fields if synthesising; client tolerates the
zero values (verified: vlouvet3 PlayerAddEvent in pkt 5 has both
fields zero).

### §2.10 — PlayerRemoveEvent (0x44c)

```
player_id = readInt(7) + 0x800
```

The +0x800 offset is the global "object id" range for players.

### §2.11 — DeltaScoreEvent (0x44d)

```
if (readFlag):
    team_id    = readInt(4)
else:
    player_id  = readInt(7) + 0x800
score          = readInt(32)
status_line    = readString
```

Either a team-score delta or a per-player-score delta. The status_line
is a server-formatted string for display.

### §2.12 — TeamAddEvent (0x44f)

```
                readInt(32)            — burned (always 0 in pcap)
team_id      = readInt(32)             — signed; -1 = "unnamed" (special unassigned-bucket team)
team_name    = readString              — e.g. "Blood Eagle"
team_skin    = readString              — e.g. "base"
```

The "unnamed" team (id=-1=0xFFFFFFFF) is always sent first in the
Phase 1 burst — it's the bucket for players who haven't yet picked
a team.

### §2.13 — MissionResetEvent (0x450)

```
(empty body)
```

Signal that the mission has been reset; client should re-fetch all
catalogue + ghost state. Triggers a follow-up Phase 1/2 burst.

### §2.14 — PlayerTeamChangeEvent (0x452)

```
player_id  = readInt(7) + 0x800
new_team   = readInt(4)
```

### §2.15 — PlayerSayEvent (0x453)

```
player_id  = readInt(12)
msg_type   = readInt(5)
msg        = readString
```

`msg_type` per `rev20/T1EventManager.h`:
- 0 = SAY_SYSTEM
- 1 = SAY_GAME
- 2 = SAY_GLOBAL
- 3 = SAY_TEAM

Server messages have `player_id = 2048` which rev20 maps to player
id 0 (the "server" pseudo-player).

### §2.16 — PlayerCommandEvent (0x455)

```
player_id  = readInt(10) + 0x800
type       = readInt(2)
if (readFlag):
    if (readFlag):
        giver_id = readInt(10) + 0x800
        readInt(6)
        if (readFlag):
            readInt(8)
        readInt(10)
        readInt(10)
if (readFlag):
    if (readFlag):
        msg = readString
```

Command-Map message (a tactical command from a commander to a player
or squad). Optional inner blocks are giver-id and command-payload
metadata.

### §2.17 — PlayerSelectCmdrEvent (0x456)

```
                readInt(7)              — player id (the chooser)
if (readFlag):
                readInt(7)              — commander id (the choice)
```

Used by the Command-Map UI to indicate which player picked which
commander.

### §2.18 — TeamObjectiveEvent (0x459)

```
line     = readInt(32)
text     = readString
```

Display a line of text on the team-objectives panel.

### §2.19 — LocSoundEvent (0x45c)

```
sound_id = readInt(8)
if (readFlag):
    readNormalVector(8)        — 17 bits (2*8+1) — direction
    readFloat(9)                — distance
```

Localised sound effect; optional position+distance for spatial audio.

### §2.20 — TargetNameEvent (0x45d)

```
target_idx  = readInt(7)
target_name = readString
```

Per-target-display string update (e.g. "Diamond Sword Flag",
"Blood Eagle Flag"). Used by the in-game HUD reticule.

### §2.21 — VoiceEvent (0x45f)

```
player_id  = readInt(7) + 0x800
wav        = readString
```

VoiceChat trigger: play this .wav file as if spoken by this player.

### §2.22 — SoundEvent (0x460)

```
readInt(10)       — sound id (10 bits, allowing 1024 distinct sounds)
readInt(2)        — type (??)
readInt(8)        — volume? duration?
```

Per `rev20/t1events.txt` (no field names attached).

### §2.23 — PingPLEvent (0x461)

```
n          = readInt(7)
loop n times:
    player_id = readInt(7) + 0x800
    ping      = readInt(8) * 4         — milliseconds, quantised to 4 ms
    pl_pct    = readFloat(6)            — packet-loss %, range [0, 1]
```

Per-tick ping/packet-loss broadcast. Emitted approximately once per
second; the pcap shows 33 PingPLEvent-first packets across 13 minutes
(roughly every 24 seconds, slightly slower than spec might suggest —
likely due to it sharing a packet with other events).

### §2.24 — PlayerSkinEvent (0x462)

```
player_id  = readInt(7) + 0x800
new_skin   = readString
```

Notify everyone that player X now wears skin "Y".

### §2.25 — DataBlockEvent (0x458)

Full details in §4 (this event has a per-type body schema that warrants
its own section).

---

## §3 — GhostManager — substream layout

### §3.1 — Scoping semantics

Per `paper §Ghost Stream Manager`:

- **Ghost-always objects** are always in scope; they're transferred
  to every newly-connected client and re-synced as their state
  changes.
- **Scope-when-visible objects** are scoped per-frame by the
  Simulation Layer's spatial database. An object that enters the
  client's view frustum (with margin for prediction) is ghosted;
  one that leaves is un-ghosted.
- Each scoped object gets a **Ghost ID** (limited-range integer) and
  a **State Mask** (per-class bit array). The state mask tracks which
  state-bits the local end wants to deliver to the ghost.

### §3.2 — Ghost-substream framing

Per `paper §Ghost Stream Manager` + pcap evidence. The GSS is a loop
of per-ghost records, terminated by a 1-bit "object-present = 0".

```
loop:
    [1 bit]   object_present (= 0 ends the loop)
    [N bits]  ghost_id        — width grows with the number of in-scope objects;
                                 see §3.4 for the dynamic-width formula
    [2 bits]  status            — 00=update, 01=create, 10=delete, 11=reserved
    [if status == create]:
        [M bits]  class_tag        — variable width (see §3.4)
        [body]   per-class create payload (see §3.5)
    [if status == update]:
        [body]   per-class state-mask-driven update payload
    [if status == delete]:
        (no body)
[1 bit]   scope_always_complete   — UNRESOLVED (see §9.1)
```

**The exact ghost_id width and per-class body schemas are NOT in any
of the consulted source.** The Frohnmayer paper documents the
algorithm at a level above the bit layout. `rev20/T1NetStream.cpp`
does not parse the GSS at all. This is the largest UNRESOLVED block
in the spec — see §9.1.

What IS known from the pcap and the existing
`CAP1-GHOST-INTRO-DECODE.md`:

- The GSS-present flag is at bit-position immediately after the
  PSC-present flag (§1.6). Verified by direct decode of pkt 132, 134,
  140 in `tah-to-public-2026-06-16.pcap`.
- A 10-byte S→C packet (pkt 140) has GSS-present=0 — the substream
  is genuinely empty. So PSC-only ticks (Move Manager state updates)
  are valid.
- A 59-byte S→C packet (pkt 303) has GSS-present=1 and ~37 bits of
  payload after the flag — consistent with one or two ghosts having
  one or two state bits each.
- The "scope-always-complete" 1-bit signal sits inside the GSS
  trailer; flipping from 0 to 1 marks the end of the scope-always
  burst (start of steady-state per-tick GSS-only ticks).

### §3.3 — Steady-state OSGB packet shapes

The S→C packets in steady state (after the catalogue dump completes
at t≈3.3 s in the pcap) take one of three shapes, distinguished by
their ESS / PSC / GSS flag triple:

| Triple | Size band | Count in pcap | Role |
|--------|-----------|---------------|------|
| ESS=1                            | 42..848 B (mean 354.8) | 222 | Reliable events: chat, score updates, ping broadcasts, datablock re-publications |
| ESS=0, PSC=1, GSS=1              | 37..425 B (mean 53.0)  | 1256 | Per-tick ghost+player-state combined |
| ESS=0, PSC=1, GSS=0 (or PSC=0, GSS=1) | 10..717 B (mean 41.4) | 385 | PSC-only (Move Manager Control-Object state) OR GSS-only ghost-update |

The **dominant shape** is GSS+PSC at ~53 bytes per packet, ~15 Hz
(every 66 ms — matching the published `current_update_delay=66`).
This is the per-tick "ghost update" stream that the TAH client
needs to keep alive once it enters gameplay.

### §3.4 — Ghost-ID width formula (paraphrased)

Per `paper §Ghost Stream Manager`: "The Ghost ID is assigned from a
limited range." The exact width is not stated in the paper but is
typically `ceil(log2(max_in_scope_objects))`. For Tribes 1 with up
to 256 in-scope objects per client the width would be 8 bits, but
real captures often show narrower fields (4..6 bits) during early
session.

**The Implementer SHOULD treat ghost_id width as a per-session
parameter** initially set to 8 bits and tuned downward if the client
rejects narrower-than-8 IDs. Alternatively, derive the width from
`build_mission_catalogue`'s maximum scope-always count + a margin.

This is an UNRESOLVED detail; see §9.1.

### §3.5 — Per-class ghost create/update bodies

Each ghost class (Player, Vehicle, Marker, StaticShape, etc.) has its
own pack/unpack methods registered with the Persist manager (`paper
§Persistent Objects`). The class is identified by a `class_tag` value
sent at create time; subsequent updates use the ghost_id alone (the
remote end looks up the class via the ghost_id dictionary).

**rev20 does NOT implement any of these.** Its T1NetStream skips
the entire GSS (`packet.writeFlag(false); // ghostmanager`). The
existing `TRIBES-GHOST-CLASSES.md` clean-room spec describes the
class-tag enumeration at the wire level (from `TAH-CLASS-TAGS.md`
analysis), but the per-class body schemas are derived from
hex-inspection and are partial.

For TAH-load-screen progression the **server does not need to emit
valid per-class GSS content**. TAH appears to accept any well-framed
GSS that consists of zero ghosts (loop terminator immediately) plus
the scope-always-complete bit set to 1. See §8 P3.

---

## §4 — DataBlockManager / catalogue bodies

### §4.1 — Datablock framing

Per `paper §Datablock Stream Manager` and `rev20/t1events.txt`:

The Datablock manager is asymmetric (server→client only) and uses
**guaranteed events** through the Event Manager to deliver catalogue
records. Each catalogue record is a `DataBlockEvent` (class 0x458,
wire tag 88). The body of a DataBlockEvent is:

```
db_type       = readInt(6)         — 0..30, see §4.2
group_size    = readInt(8)         — total records expected in this group
block_index   = readInt(8)         — 0 .. group_size-1, or 0xff = sentinel
if (block_index != 0xff):
    <per-type body — see §4.3..§4.30>
```

The 6+8+8 header is **reaffirmed** against the wire by direct decode
of every DataBlockEvent in `tah-to-public-2026-06-16.pcap` (1300+
records across 30 groups for the Blastside mission). The
`TRIBES-DATABLOCKS.md` §2 hypothesis is correct; the
`TRIBES-CATALOGUE-BODIES.md` "block stuck at 0" anomaly was a
cap1-specific corruption (or possibly a misaligned bit-read in the
prior decoder), NOT a protocol difference.

**Sentinel rule (`block_index == 0xff`):** an empty group is
communicated by sending one DataBlockEvent with
`(group_size=0, block_index=0xff)` and no body. The Blastside session
uses this for groups 9 (VehicleData), 11 (TankData), 12 (HoverData),
13 (ProjectileData), 18 (InteriorShapeData), 27 (CarData), 30
(IrcChannelData) — see §4.31.

**dataFinished detection:** the client signals "all catalogue
records ingested" by sending a `dataFinished` SimConsoleEvent
remoteEval (§2.6). In `rev20/T1EventManager.cpp:DataBlockEvent_Unpack`
there's a special case: when `db_type == 30` (IrcChannelData) AND
`(block_index == 0xff || block_index == group_size - 1)`, it
fires `OnDataFinished()` (which in the rev20 client triggers
`remoteEval("dataFinished")` automatically). So the **server's
final catalogue record MUST be group 30 (IrcChannelData)** — either
populated or sentinel — to trigger the client's dataFinished
emission.

Verified in the pcap: the last DataBlockEvent in pkt 129 (t=3.302 s)
is `IrcChannelData (group=30) max=0 blk=255` (sentinel). The C→S
`dataFinished` remoteEval arrives at pkt 130 (t=3.310 s) — 8 ms
later.

### §4.2 — Datablock type enum

Per `rev20/T1EventManager.h:#define`:

| db_type | Name              | Inherits           | Body in §  |
|---------|--------------------|---------------------|------------|
| 0       | SoundProfileData   | (none)              | §4.4       |
| 1       | SoundData          | (none)              | §4.5       |
| 2       | DamageSkinData     | (none)              | §4.6       |
| 3       | ArmorData          | StaticData          | §4.7       |
| 4       | StaticShapeData    | StaticData          | §4.8       |
| 5       | ItemData           | StaticData          | §4.9       |
| 6       | ItemImageData      | GameBaseData        | §4.10      |
| 7       | MoveableData       | StaticShapeData     | §4.11      |
| 8       | SensorData         | StaticShapeData     | §4.12      |
| 9       | VehicleData        | StaticShapeData     | §4.13      |
| 10      | FlierData          | VehicleData         | §4.14      |
| 11      | TankData           | VehicleData         | (sentinel-only in Blastside; layout per rev20 maps to VehicleData) |
| 12      | HoverData          | VehicleData         | (sentinel-only in Blastside) |
| 13      | ProjectileData     | ShapeBaseData       | §4.15 |
| 14      | BulletData         | ProjectileData      | §4.16 |
| 15      | GrenadeData        | ProjectileData      | §4.17 |
| 16      | RocketData         | ProjectileData      | §4.18 |
| 17      | LaserData          | ProjectileData      | §4.19 |
| 18      | InteriorShapeData  | (none)              | (sentinel-only in Blastside; layout not in rev20) |
| 19      | TurretData         | SensorData          | §4.20 |
| 20      | ExplosionData      | GameBaseData/ShapeBase | §4.21 |
| 21      | MarkerData         | GameBaseData        | §4.22 |
| 22      | DebrisData         | GameBaseData        | §4.23 |
| 23      | MineData           | ItemData (aliased) | §4.24 |
| 24      | TargetLaserData    | LaserData (aliased) | §4.25 |
| 25      | SeekingMissileData | ProjectileData     | §4.26 |
| 26      | TriggerData        | GameBaseData       | §4.27 |
| 27      | CarData            | VehicleData        | (sentinel-only in Blastside) |
| 28      | LightningData      | ProjectileData     | §4.28 |
| 29      | RepairEffectsData  | LightningData (aliased) | §4.29 |
| 30      | IrcChannelData     | GameBaseData       | §4.30 (terminator — see §4.1) |

### §4.3 — Base bodies

Several types share common base bodies. They are NOT independent
catalogue types; they're inheritance fragments read in sequence
before the derived type's own fields.

#### GameBaseData

```
                readSignedInt(8)         — sign-mag, range [-127, 127]; usually 0 or 1
map_icon      = readString
class_name    = readString
```

#### ShapeBaseData

```
shape         = readString
if (shape[0] == 0xfe || shape[0] == 0xff):
                readInt(32)              — burned (presumably a 32-bit data-file ref)
```

The 0xfe/0xff prefix is a sentinel for "shape stored as data-file
reference, not by name". In the pcap most ShapeBaseData strings are
short ASCII names like `"bullet.DTS"`.

#### StaticData

```
read GameBaseData
read ShapeBaseData

shield_name = readString                — usually empty
                burnBits(96)             — 12 bytes of opaque transform/bound data
for i in 0..15:
    if (readFlag):
        readString                       — action name
        readInt(8)                       — action index
                readFlag                  — final flag
```

The for-loop reads up to 16 "action slots" (animation/sound/effect
keys for the static shape). Each slot present iff the leading flag is
1. Per the pcap most slots are empty (`readFlag()` returns 0).

#### StaticShapeData (struct, derived from StaticData)

```
read StaticData

readInt(8)                              — burned
readFlag
readFlag
```

#### VehicleData

```
read StaticShapeData
burnBits(632)                           — 79 bytes of opaque physics constants
```

#### ProjectileData

```
read GameBaseData
read ShapeBaseData
burnBits(546)                           — 69 bytes of opaque ballistic constants
```

### §4.4 — SoundProfileData (db_type 0)

```
                readInt(6)               — sound-class id
                readFloat(10)            — base volume
if (readFlag):  readFloat(10)            — volume randomisation range
if (readFlag):  readFloat(10)            — pitch randomisation range
if (readFlag):  readNormalVector(10)     — emission direction (21 bits)
if (readFlag):  burnBits(32)             — attenuation curve param 1
if (readFlag):  burnBits(32)             — attenuation curve param 2
if (readFlag):  burnBits(32)             — attenuation curve param 3
```

Mean body width in pcap: 90..108 bits (varies by which optional
fields are set).

### §4.5 — SoundData (db_type 1)

```
name      = readString                  — e.g. "Land_On_Ground.wav"
            readFloat(6)                — base volume multiplier
            readInt(8)                  — sound-profile id (ref into group 0)
```

Mean body width: 100..146 bits. Varies primarily with the length of
the .wav filename.

### §4.6 — DamageSkinData (db_type 2)

```
for i in 0..9:
    skin[i] = readString
```

10 skin filenames — one per damage-level / armour-skin combination.

### §4.7 — ArmorData (db_type 3)

```
read StaticData
jet_flame = readString                  — e.g. "jet1.DTS" or empty

for i in 0..50:                          — 51 animation nodes
    node_name = readString
    if (readFlag):
        readInt(8)                       — node-attached-shape id
    readFlag                              — bool flag (likely "mirror node")
    readInt(4)                            — left-side mask
    readInt(4)                            — right-side mask

burnBits(807)                            — ~101 bytes of opaque per-armor stats
```

### §4.8 — StaticShapeData (db_type 4) — body

```
read StaticData
readInt(8)
readFlag
readFlag
```

### §4.9 — ItemData (db_type 5)

```
read StaticData
burnBits(224)                            — 28 bytes opaque
                readString                — typically empty
                readFlag
                readFlag
                readFlag
                readString                — alternate icon? empty in pcap
burnBits(192)                            — 24 bytes opaque
```

### §4.10 — ItemImageData (db_type 6)

```
read GameBaseData
item       = readString                  — item type name
burnBits(849)                            — 107 bytes opaque (model + transform + offsets)
```

### §4.11 — MoveableData (db_type 7)

```
read StaticShapeData
burnBits(67)                              — ~9 bytes opaque
```

### §4.12 — SensorData (db_type 8)

```
read StaticShapeData
burnBits(66)                              — ~9 bytes opaque (range, FOV)
```

### §4.13 — VehicleData (db_type 9)

```
read StaticShapeData
burnBits(632)                             — 79 bytes opaque physics
```

In Blastside this group is empty (sentinel-only); FlierData (10) and
TankData (11) — derived from VehicleData — carry the real entries.

### §4.14 — FlierData (db_type 10)

```
read VehicleData
burnBits(224)                             — 28 bytes opaque flight params
```

### §4.15 — ProjectileData (db_type 13) — body

```
read GameBaseData
read ShapeBaseData
burnBits(546)                             — 69 bytes opaque ballistics
```

In Blastside this group is empty; BulletData (14), GrenadeData (15),
etc. — all derived — carry the actual projectile defs.

### §4.16 — BulletData (db_type 14)

```
read ProjectileData
burnBits(96)                              — 12 bytes opaque
```

### §4.17 — GrenadeData (db_type 15)

```
read ProjectileData
burnBits(32)                              — 4 bytes opaque
trail   = readString                      — e.g. "smoke.DTS"
```

### §4.18 — RocketData (db_type 16)

```
read ProjectileData
burnBits(32)                              — 4 bytes opaque
trail   = readString                      — e.g. "rsmoke.DTS"
burnBits(64)                              — 8 bytes opaque
```

### §4.19 — LaserData (db_type 17)

```
read ProjectileData
pulse_bitmap = readString                — e.g. "laserpulse.bmp"
hit_model    = readString                 — e.g. "laserhit.DTS"
                readFlag
```

### §4.20 — TurretData (db_type 19)

```
read SensorData
burnBits(225)                             — 29 bytes opaque turret params
```

### §4.21 — ExplosionData (db_type 20)

```
read GameBaseData
read ShapeBaseData
burnBits(66)                              — ~9 bytes opaque
if (readFlag):  burnBits(480)            — 60 bytes opaque
                readFlag
```

### §4.22 — MarkerData (db_type 21)

```
read GameBaseData
icon  = readString                       — HUD-icon filename
```

Lightweight; bodies in the pcap are ~120 bits each.

### §4.23 — DebrisData (db_type 22)

```
read GameBaseData
burnBits(32)                              — 4 bytes opaque
                readString                — typically empty
burnBits(707)                             — 89 bytes opaque
```

### §4.24 — MineData (db_type 23)

```
read ItemData                             — see §4.9
```

Aliased through ItemData.

### §4.25 — TargetLaserData (db_type 24)

```
read LaserData                             — see §4.19
```

### §4.26 — SeekingMissileData (db_type 25)

```
read ProjectileData
burnBits(96)                              — 12 bytes opaque tracking params
```

### §4.27 — TriggerData (db_type 26)

```
read GameBaseData
```

### §4.28 — LightningData (db_type 28)

```
read ProjectileData
burnBits(192)                             — 24 bytes opaque
bmp   = readString                        — e.g. "lightningNew.bmp"
```

### §4.29 — RepairEffectsData (db_type 29)

```
read LightningData
```

### §4.30 — IrcChannelData (db_type 30) — TERMINATOR

```
read GameBaseData
```

When this datablock is received (either as a real entry or a
sentinel), the client fires its internal `OnDataFinished()` hook,
which the TAH client uses to push the `dataFinished` SimConsoleEvent
remoteEval upstream. **A valid TAH-compatible catalogue MUST end
with an IrcChannelData record (sentinel suffices).**

### §4.31 — Sentinel-only groups (Blastside)

The following groups carry only a sentinel record in the Blastside
session — they exist so the client's catalogue walker sees one record
per group_id and advances:

| group | name              |
|-------|-------------------|
| 9     | VehicleData       |
| 11    | TankData          |
| 12    | HoverData         |
| 13    | ProjectileData    |
| 18    | InteriorShapeData |
| 27    | CarData           |
| 30    | IrcChannelData    |

Each sentinel record is exactly 6+8+8 = 22 bits of header
(`db_type=G, group_size=0, block_index=0xff`) plus 10 bits of event
header = ~32 bits ≈ 4 bytes per sentinel. The Implementer SHOULD emit
sentinels for every group that has no real records in the mission, so
the catalogue dump traverses all 31 group IDs that the client expects.

### §4.32 — Full Blastside catalogue inventory

Cross-checking the pcap against the t1events.txt enumeration, the
public server's Blastside session emits these group counts:

| db_type | name              | group_size | blocks observed |
|---------|--------------------|------------|------------------|
| 0       | SoundProfileData   | 10         | 0..9            |
| 1       | SoundData          | 153        | 0..152          |
| 2       | DamageSkinData     | 2          | 0..1            |
| 3       | ArmorData          | 5          | 0..4            |
| 4       | StaticShapeData    | 52         | 0..51           |
| 5       | ItemData           | 45         | 0..44           |
| 6       | ItemImageData      | 23         | 0..22           |
| 7       | MoveableData       | 36         | 0..35           |
| 8       | SensorData         | 5          | 0..4            |
| 9       | VehicleData        | 0          | sentinel        |
| 10      | FlierData          | 3          | 0..2            |
| 11      | TankData           | 0          | sentinel        |
| 12      | HoverData          | 0          | sentinel        |
| 13      | ProjectileData     | 0          | sentinel        |
| 14      | BulletData         | 5          | 0..4            |
| 15      | GrenadeData        | 3          | 0..2            |
| 16      | RocketData         | 2          | 0..1            |
| 17      | LaserData          | 1          | 0..0            |
| 18      | InteriorShapeData  | 0          | sentinel        |
| 19      | TurretData         | 7          | 0..6            |
| 20      | ExplosionData      | 19         | 0..18           |
| 21      | MarkerData         | 3          | 0..2            |
| 22      | DebrisData         | 7          | 0..6            |
| 23      | MineData           | 2          | 0..1            |
| 24      | TargetLaserData    | 1          | 0..0            |
| 25      | SeekingMissileData | 1          | 0..0            |
| 26      | TriggerData        | 1          | 0..0            |
| 27      | CarData            | 0          | sentinel        |
| 28      | LightningData      | 2          | 0..1            |
| 29      | RepairEffectsData  | 1          | 0..0            |
| 30      | IrcChannelData     | 0          | sentinel-only (terminates the catalogue) |

**This table closes R-7.1.7 OQ-1..OQ-6** from
`TRIBES-PROTOCOL-PCAP-DIFF.md` §9. Every type's body schema is
specified above; every group's expected count is enumerated here.

### §4.33 — Burst pacing

Per `paper §Datablock Stream Manager`:

> "Since datablocks are only transferred at times when the simulation
> is effectively stopped, more bandwidth is available and datablocks
> can contain more information than would normally be possible."

The public TAH server delivers the full ~430-record Blastside
catalogue in ~50 packets across ~3 s (matching its
`max_packet_size=450, max_update_delay=33` rate-control declared in
pkt 5 + the throttle to `cur_size=400, cur_delay=66` declared in pkt
6). Catalogue records are interleaved with the join-time Phase 1
events. The Implementer SHOULD:

1. Set `R1=1, max_delay=33, max_size=450` in the first reply.
2. Set `R0=1, cur_delay=66, cur_size=400` in the second reply.
3. Drain the catalogue at ~one packet per `cur_delay` until exhausted.
4. End the final catalogue packet with the IrcChannelData sentinel.

---

## §5 — Move Manager — input + state protocol

### §5.1 — Architecture

Per `paper §Move Stream Manager`:

The Move Manager is asymmetric:
- **Client→server:** input moves (forward/back/strafe/jump/jet/fire +
  yaw/pitch). Gathered every **32 ms** (matching the simulation tick).
- **Server→client:** Control-Object state (the player's avatar
  position/velocity/orientation), "soonest possible" delivery — i.e.
  packed into every server-emitted packet.

Both directions use a **sliding window** of moves. The client tracks
unacked moves in a window; the server advances it via per-packet
ack of "highest move number received".

### §5.2 — Player Move packet (C→S)

Per `rev20/t1demoformat.txt`:

```
              0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
            +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
   0000:   | Forward     | Backward     | Left        | Right        |
            +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
   0010:   |JET|? |JMP|GUN|       ?              | Yaw         | Pitch       |
            +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
```

The diagram is 32 bytes-per-row in the ASCII; per the demo format
the **first 4 bytes** are 1-byte trigger states for the four
movement directions (Forward / Backward / Left / Right). The **next
byte** (5) is a packed bit field:

```
bit 0: JET trigger
bit 1: ? (reserved)
bit 2: JMP trigger
bit 3: GUN trigger
bits 4..7: ?
```

Followed by:

- byte 6: ? (likely additional trigger bits or move index low byte)
- byte 7: Yaw (8-bit signed delta?)
- byte 8: Pitch (8-bit signed delta?)

This is the **demo-file** Player Move format. Per `t1demoformat.txt`,
demos store player-move packets verbatim (the same bytes that the
client sent to the server). So the wire format and the demo format
are identical.

The actual C→S DataPacket carrying moves wraps these bytes inside the
PSC substream — see §5.3.

### §5.3 — Wire-level C→S move payload

In the pcap, every C→S DataPacket of size 15..19 B has the substream
flags `ESS=0, PSC=1, GSS=0`. The PSC substream then carries the move
payload. Sample bytes from pkt 202 (16 B):

```
bb 61 b9 00  c8 9c 0a 00  00 10 00 00  30 0c 83 02
```

After the 32-bit VC header and 5 bits of {R0, R1, ESS, PSC, GSS}
flags, the bit cursor is at bit 37. The remaining 88 bits (11 bytes)
are the PSC substream payload. **The exact bit-layout of the PSC
substream is NOT in any consulted document.** Likely contents per
`paper §Move Manager`:

- 3..6 bits: number of moves in this packet (typical 1..3)
- per-move:
  - 1 bit: move-present
  - per-move-body: ~30-40 bits encoding directional input + yaw/pitch

The Implementer should defer full C→S PSC decode to a follow-up
spec; for TAH-load-screen progression the **server only needs to
ack the C→S DataPacket** (advancing its `hrcv` and including it in
the next outgoing ack-run). Server-side processing of the moves can
be a no-op until gameplay is reached.

### §5.4 — Wire-level S→C move payload

Per `paper §Move Manager`:

> "Control Object state data write...into every packet sent from the
> server. This combination of state transfer and deterministic move
> processing is used to keep the Control Object and its ghost
> synchronised."

In the pcap, S→C DataPackets with `ESS=0, PSC=1, GSS=0` carry the
Control-Object state. Pkt 140 (10 B) decodes to a 43-bit PSC payload
after the substream flags — consistent with a compact position +
velocity + orientation packed encoding (e.g. 3×8-bit position quantum +
3×4-bit velocity quantum + 1-byte orientation index ≈ 40 bits).

**The exact S→C PSC format is UNRESOLVED.** For TAH-load-screen
progression the Implementer SHOULD emit either:

1. An empty PSC substream (PSC=0) — the client tolerates no Control-
   Object update for a few ticks but eventually times out the avatar.
2. A copy of a captured S→C PSC payload (43 bits from pkt 140) on
   every tick — this passes the bit-count check at the client and
   the Control-Object will appear frozen at the captured pose.

Option 1 is simpler. Option 2 is more progress-preserving.

### §5.5 — The `class=8 Marker` issue (OQ-4)

In `TRIBES-PROTOCOL-PCAP-DIFF.md` §9 OQ-4, the prior Reader noted
"class=8 Marker" as UNRESOLVED. This was a bit-position misread:
wire-tag 8 = SimConsoleEvent (§2.6), not anything Marker-related.
Markers are catalogue group 21 (db_type 21, see §4.22). **OQ-4 is
RESOLVED:** there is no separate class-tag-8 Marker event; the
mis-named "Marker" referred to db_type 21 catalogue entries.

---

## §6 — Connection-layer ack notification

### §6.1 — Sliding window — receiver side

Per `paper §Connection Layer` + the existing pcap-diff §1 header
table + rev20's processing in `T1Connection` (paraphrased):

The receiver maintains a 32-entry bitmap covering its window of
inbound sequence numbers. When a packet with `send_seq = S` arrives:

1. If `S` is the next expected seq (`hrcv + 1`), advance `hrcv` to
   `S` and clear the corresponding bitmap entry.
2. If `S` is **further forward** than expected (some packets
   dropped/reordered), advance `hrcv` to `S` AND mark the missing
   bitmap entries as "to-be-acked-via-run-list".
3. If `S` is **behind** `hrcv` but within the 32-entry window, mark
   that entry in the run-list explicitly (it was previously thought
   dropped and now arrived — possibly out of order).
4. If `S` is older than the window, discard (already considered lost).

Every outgoing packet carries:
- `hrcv` (5 bits, cumulative ack of all packets ≤ this seq).
- Zero or more run-list entries (3-bit count, 5-bit start) for explicit
  acks of non-contiguous receives ahead of `hrcv`.

### §6.2 — Sliding window — sender side notification

When a packet with `send_seq = S` is acked (either via cumulative
`hrcv` advance OR via a run-list entry containing S), the Connection
Manager fires `OnPacketNotify(delivered=true, S)` to the Stream layer.
When a packet `S` is provably lost (a packet with seq > S+window was
acked without S in the run-list), `OnPacketNotify(delivered=false, S)`
fires.

Per `rev20/T1NetStream.cpp:OnPacketNotify`, the only manager that
reacts is the EventManager (`rev20/T1EventManager.cpp:PacketNotify`).
On `delivered=false` it migrates the events from that packet's
TransmissionRecord queue back onto the front of the current outgoing
queue (i.e. re-queues them for next-packet retransmission). On
`delivered=true` it discards them.

### §6.3 — Ack-only packets

Pure-ack packets carry the VC header + a 5-bit "Ack" ptype (= 16
per `TRIBES-NETPROTO.md` §3.2; the live wire uses ptype=2 for AC-ack,
ptype=16 isn't observed in the pcap as a top-level ptype but the
`rev20/T1Connection.h:PACKET_ACKED` value is 5 in their enum — see
the conflict notes in §1.7). The Implementer should accept both
ptype values and emit ptype=0 (DataPacket with no payload — i.e.
ESS=0, PSC=0, GSS=0) as the default ack form, which is what the
public server does on quiet ticks.

The 4-byte pure-ack form seen in pkt 3 (TAH ACKing the AC):

```
07 08 09 80
```

Decodes as:
- bit 0: vc=1
- bit 1: parity=1
- bits 2..10: send_seq=2 (wait — this is the AC ack which has
  send_seq=1; the leading byte `07` is `0000 0111` which read LSB-first
  decodes to vc=1, par=1, send_seq[lo3]=001, so send_seq=1 if hrcv has
  no high bits... see TRIBES-NETPROTO.md §3 for the full bit-walk; this
  spec defers to that one for the connect-handshake bit positions).

### §6.4 — Ping reply

Per `paper §Connection Layer` and `TRIBES-NETPROTO.md` §3.8: an
otherwise-idle connection sends a `Ping` (ptype=7) carrying only the
VC header. The recipient replies with an `Ack` (ptype=16) carrying
only the VC header. Both bypass the Stream layer entirely.

**Our server is missing the Ping-reply handling.** When the public
TAH server's session goes idle (no events, no GSS, no PSC for >5 s),
the TAH client sends a Ping; our server doesn't recognise ptype=7
and drops it. This isn't blocking the load screen (the load screen
finishes before the keepalive timeout) but it WILL block any
sustained session.

Implementer should add ptype=7 handling: on receipt, emit a 4-byte
ack (`vc=1, parity=p, send_seq=next, hrcv=their_seq, no_runs,
ptype=16`) — i.e. the same 4-byte form as the C→S pure ack.

### §6.5 — Retransmission rules per data classification

Per `paper` (synthesised from §Connection Layer, §Event Stream
Manager, §Ghost Stream Manager, §Move Stream Manager):

| Classification | On drop notification | Bytes "owned" by which manager |
|----------------|----------------------|---------------------------------|
| Non-guaranteed | (do nothing) | Event manager (some events are non-guaranteed); Ghost manager (state bits where loss-was-stale) |
| Guaranteed     | Re-queue at head of outgoing queue | Event manager (DataBlockEvent, PlayerSayEvent, etc.) |
| Most-recent-state | Re-set the dirty-bit IFF no subsequent packet already carried newer state | Ghost manager state masks; Move manager Control-Object state |
| Guaranteed Quickest | Write into every packet until acked | Move manager input moves |

In practice the **only** retransmit our server needs to implement for
TAH-progression is the Event-Manager guaranteed-retransmit (re-queue
the lost-packet's events at the head of the next outgoing queue).

---

## §7 — Initial connection sequence — bit-by-bit walk

Walk of the first ~30 packets of `tah-to-public-2026-06-16.pcap`,
TAH (`192.168.1.101:61429`) ↔ public TAH server (`104.128.49.65:28006`).

### §7.1 — Pkt 1 (t=0.000s) — RequestConnect (50 B, C→S)

```
07 00 17 4f 87 bf f3 18 13 8a 63 1d 01 00 0d 56 ...
```

Shape F per `TAH-REQUESTCONNECT-SHAPES.md`. Carries nonce
`13 8a 63` at bytes 8..10 and separator byte `0x1d` at byte 11.
Already documented; brief recap only.

### §7.2 — Pkt 2 (t=0.028s) — AcceptConnect (16 B, S→C)

```
07 00 09 60 13 8a 63 1d 0a 08 00 00 00 08 00 00
```

Already documented byte-for-byte in `TRIBES-PROTOCOL-PCAP-DIFF.md` §2.
The encoder is implemented in
`open-siege/engine/src/server_listener.cpp:build_tah_accept_connect_reply`.
**Already correct** in the current codebase (P0 from pcap-diff
roadmap is done).

### §7.3 — Pkt 3 (t=0.028s) — Pure ack (4 B, C→S)

```
07 08 09 80
```

TAH acknowledges receipt of the AC. Sent immediately. The 4-byte
form encodes: vc=1, parity=1, send_seq=1, hrcv=1, no runs, ptype=16
(Ack). Our server should treat this as a no-op (the Connection
manager processes it; nothing for the Stream layer to do).

### §7.4 — Pkt 4 (t=0.042s) — TAH ClientReady (62 B, C→S)

The most informative packet for "what TAH wants to see during
handshake". Bit walk:

| Bits | Width | Field | Value |
|------|-------|-------|-------|
| 0..0   | 1  | vc      | 1 |
| 1..1   | 1  | parity  | 1 |
| 2..10  | 9  | send_seq | 2 |
| 11..15 | 5  | hrcv    | 1 |
| 16..18 | 3  | ack-run cnt | 1 |
| 19..23 | 5  | ack-run start | 1 |
| 24..26 | 3  | ack-term | 0 |
| 27..31 | 5  | ptype   | 0 (DataPacket) |
| 32..32 | 1  | R0 | 0 |
| 33..33 | 1  | R1 | 1 |
| 34..43 | 10 | max_update_delay (ms) | 66 |
| 44..53 | 10 | max_packet_size (B) | 400 |
| 54..54 | 1  | ESS-present | 1 |

Then 2 SimConsoleEvent events:

**Event 0** (bits 55..174, 119 body bits):

| Bits | Field | Value |
|------|-------|-------|
| 55 | event-present | 1 |
| 56 | flag_a | 1 |
| 57 | flag_b | 0 |
| 58 | flag_c (has_eseq) | 1 |
| 59..65 | explicit_seq | 0 |
| 66..72 | wire_tag | 8 (= 0x408 SimConsoleEvent) |
| 73..77 | argc | 1 |
| 78..174 | string arg[0] | `"zAdminActiveMode"` (Huffman) |

**Event 1** (bits 184..450, 256 body bits):

| Bits | Field | Value |
|------|-------|-------|
| 175 | event-present | 1 |
| 176..183 | flags (continuous) | sc=1 |
| 184..190 | wire_tag | 8 |
| 191..195 | argc | 10 |
| 196..onward | strings | `"SetCLInfo"`, `"Entities/base"`, `""`, `""`, `""`, `""`, `""`, `"2"`, `"false"`, `"-1"` |

Then ESS terminator + PSC=0 + GSS=0 + padding.

**The Implementer's server-side handler** for the TAH ClientReady
should:

1. Parse the VC header, validate parity & advance `hrcv`.
2. Process R1=1 with max_delay=66, max_size=400 → update the
   per-session rate-control state (this is TAH's voluntary throttle
   declaration; client running on a 400 B / 66 ms budget).
3. Walk the ESS, recognise both SimConsoleEvent remoteEvals, and
   trigger the session's "client is ready" state.
4. **Specifically: detect the `SetCLInfo` remoteEval** as the
   ClientReady signal. Its first argument is the entity pack name
   ("Entities/base" = the stock 1.41 freeware pack); subsequent args
   are password, etc.
5. Reply with Phase 1 (TeamAddEvents + PlayerAddEvent + SVInfo +
   MODInfo) in the first server reply, plus rate-control declaration
   R1=1, max=33/450.

### §7.5 — Pkt 5 (t=0.054s) — Server Phase 1 reply (285 B, S→C)

The server's first DataPacket. Bit walk:

| Bits | Field | Value |
|------|-------|-------|
| 0..31 | VC header (seq=2, hrcv=1, ack cnt=1 start=1, ptype=0) | — |
| 32 | R0 | 0 |
| 33 | R1 | 1 |
| 34..43 | max_update_delay | 33 |
| 44..53 | max_packet_size | 450 |
| 54 | ESS-present | 1 |

Events (6 total):

| # | wire_tag | name | content |
|---|----------|------|---------|
| 0 | 79 | TeamAddEvent | id=-1, name="unnamed", skin="base" |
| 1 | 79 | TeamAddEvent | id=0, name="Blood Eagle", skin="base" |
| 2 | 79 | TeamAddEvent | id=1, name="Diamond Sword", skin="base" |
| 3 | 75 | PlayerAddEvent | player_id=N, name="vlouvet3", team=-1 |
| 4 | 8 | SimConsoleEvent | `SVInfo("1.11", "Rosco's Mixtape...", "base", ...)` |
| 5 | 8 | SimConsoleEvent | `MODInfo("Server is optimized with the <f2>Pickup<f2>...")` |

This packet sets up the team/player UI and identifies the server
build. The Implementer should emit equivalent content on its first
reply:

- TeamAddEvent for each team (unnamed, Blood Eagle, Diamond Sword for
  CTF Blastside).
- PlayerAddEvent for the joining player.
- SimConsoleEvent `SVInfo` to publish version + welcome message.
- SimConsoleEvent `MODInfo` for any server-specific mod string.

Skin names default to `"base"`.

### §7.6 — Pkt 6 (t=0.119s) — Phase 2 first packet (424 B, S→C)

VC header: seq=3, hrcv=1, ack cnt=1 start=2, ptype=0. R0=1
cur_delay=66 cur_size=400. R1=0. ESS=1.

Events (29 total):
- 1× SimConsoleEvent (`FileURL("")`) — opens Phase 2; this is the
  HTTP file-download URL prefix (empty here = no external downloads).
- 10× DataBlockEvent (`SoundProfileData[0..9]`).
- 18× DataBlockEvent (`SoundData[0..17]`).

The packet end is exactly at ESS-terminator + PSC=0 + GSS=0 + zero
pad to byte 424.

### §7.7 — Pkts 7–129 — Catalogue dump

124 more packets, alternating S→C catalogue records and C→S small
acks. Catalogue traversed in db_type order: SoundData (~150
records across 10 packets), DamageSkin, Armor, StaticShape (52
entries across 7-9 packets), Item, ItemImage, Moveable, Sensor,
Vehicle(sentinel), Flier, Tank/Hover/Projectile(sentinels), Bullet,
Grenade, Rocket, Laser, InteriorShape(sentinel), Turret, Explosion,
Marker, Debris, Mine, TargetLaser, SeekingMissile, Trigger,
Car(sentinel), Lightning, RepairEffects, **IrcChannelData (sentinel,
terminator)**.

### §7.8 — Pkt 130 (t=3.310s) — TAH dataFinished (27 B, C→S)

TAH sends `SimConsoleEvent { cmd="dataFinished" }` as a single-event
DataPacket. This is the explicit "I've consumed the catalogue;
proceed" signal. The trigger (per §4.1) is the client's
DataBlockEvent_Unpack noticing db_type=30 sentinel and firing
OnDataFinished_Chain.

### §7.9 — Pkts 131..150 — Transition to steady state

The server emits:

- Pkts 132 (467 B) and 134 (420 B): ESS=0 with PSC=1 — the
  initial player Control-Object state delivery + any pending PSC
  data. (467 B is unusually large here; it likely also carries the
  scope-always ghost intros via GSS, but our decoder didn't bottom out
  the GSS schema — see §9.1.)
- Pkts 137 (356 B) and 140-150 (10 B each): ESS=0 PSC=1 GSS=0 (or
  GSS=1 on the 10 B ticks). The 10 B form is the steady-state
  Move-Manager Control-Object state delta, sent every ~66 ms.

This is the **load-screen completion**: TAH transitions from "loading"
to "in-game" UI when it has received enough scope-always ghosts to
populate the world (estimated 14-16 s into the session per
`TRIBES-PROTOCOL-PCAP-DIFF.md` §4.4) AND when its Move-Manager has
received its first Control-Object state update.

### §7.10 — UI transition signal

Per the wire evidence, TAH transitions to the load-screen UI as soon
as it receives the AC reply (pkt 2) — load-screen is the UI shown
during the burst. It transitions to **in-game UI** after:

1. ClientReady (its SetCLInfo) is acked (implicit, via the server's
   pkt 5 ack-run).
2. The entire catalogue is received (signalled internally by
   IrcChannelData sentinel).
3. The dataFinished remoteEval is sent and acked.
4. The first Control-Object state is received from the server
   (PSC-bearing packet).

Steps 1-3 happen by t=3.310 s in the public pcap. Step 4 happens at
t≈3.369 s (pkt 132). The load → in-game transition occurs around
t=3.4..3.5 s.

**The blocker on our server isn't reaching step 4 yet — it's failing
at step 1 (because the catalogue dump is broken) or step 3 (because
no IrcChannelData sentinel ever gets emitted).** See §8.

---

## §8 — Implementer roadmap

Prioritized in TAH-progression-impact order. Code refs are
`open-siege/engine/src/...`.

### P0 — DONE: 16 B AcceptConnect

Status: ALREADY IMPLEMENTED.
- `server_listener.cpp:build_tah_accept_connect_reply` (lines 250..271)
  emits the correct 16-byte form per `TRIBES-PROTOCOL-PCAP-DIFF.md`
  §2.

### P1 — Emit a valid Phase 1 reply on first DataPacket from TAH

**File:** `engine/src/tah_burst_orchestrator.cpp` →
`build_initial_burst()`.

**Current behavior:** emits ~30 packets in a single burst at AC-time.

**Target behavior:** on receipt of TAH's ClientReady DataPacket (the
62 B packet at §7.4), respond with a Phase 1 packet that:
- Sets `R1=1, max_update_delay=33, max_packet_size=450` (publish
  server's intended rate).
- Carries Event-SubStream content:
  - 3× `TeamAddEvent` (unnamed/-1, Blood Eagle/0, Diamond Sword/1).
  - 1× `PlayerAddEvent` for the joining slot (name from
    `SetCLInfo` arg 0 if extractable, else a generated name).
  - 2× `SimConsoleEvent` for SVInfo + MODInfo. SVInfo args are:
    `["1.11", server_motd, "base", server_long_desc, ""]`. MODInfo
    args are: `[mod_description]`.

Per §2.12 (TeamAddEvent) and §2.9 (PlayerAddEvent) for byte layouts.

**Complexity: M.** Requires implementing the Event Manager's outbound
encoder for SimConsoleEvent and TeamAddEvent + PlayerAddEvent.

**Cross-references:** §2, §7.5.

### P2 — Emit the catalogue as DataBlockEvents

**File:** `engine/src/tah_burst_orchestrator.cpp` +
`engine/src/tah_default_catalogue.cpp`.

**Current behavior:** emits a minimal scope-always-intro burst using
hand-tuned class tags (`kTahStaticShapeClassTag` etc.) and a "minimal
catalogue" of placeholders.

**Target behavior:** after the Phase 1 packet, emit the full
Blastside catalogue as a sequence of DataBlockEvent (wire tag 88)
records:

1. For each db_type in 0..30 (in order):
   - If the mission uses this type, emit one DataBlockEvent per
     block in the group (block_index=0..group_size-1) carrying the
     per-type body (per §4.4..§4.30).
   - Otherwise emit a single sentinel DataBlockEvent
     (`group_size=0, block_index=0xff`, no body — see §4.31).
2. Pack records into packets of ≤ `cur_size` (400 B) bytes each,
   one packet per `cur_delay` (66 ms).
3. The catalogue MUST end with a db_type=30 (IrcChannelData) record
   (sentinel suffices) to trigger the client's `dataFinished` signal.

For TAH-progression we do NOT need to emit byte-accurate per-type
bodies — opaque bit-skips of the right total width per record body
suffice (e.g. SoundProfileData = `readInt(6)` + `readFloat(10)` +
6× optional flags, average 90 bits). Real asset data can be a
follow-up spec.

**Stock CTF catalogue counts** (use these for any CTF mission until
per-mission counts are tabulated):

```
SoundProfileData: 10  | SoundData:        153
DamageSkinData:   2   | ArmorData:        5
StaticShapeData:  ~52 | ItemData:         ~45
ItemImageData:    ~23 | MoveableData:     ~36
SensorData:       5   | VehicleData:      0 (sentinel)
FlierData:        3   | TankData:         0 (sentinel)
HoverData:        0 (sentinel) | ProjectileData:   0 (sentinel)
BulletData:       5   | GrenadeData:      3
RocketData:       2   | LaserData:        1
InteriorShapeData: 0 (sentinel) | TurretData:       7
ExplosionData:    19  | MarkerData:       3
DebrisData:       7   | MineData:         2
TargetLaserData:  1   | SeekingMissileData: 1
TriggerData:      1   | CarData:          0 (sentinel)
LightningData:    2   | RepairEffectsData: 1
IrcChannelData:   0 (sentinel)
```

**Complexity: L.** This is the big one — the Implementer must emit
~430 DataBlockEvent records across ~50 packets. Body emission can be
all-zeros padding to the right bit-width for each type.

**Cross-references:** §4, §7.7.

### P3 — Add the dataFinished handler + steady-state PSC/GSS emission

**File:** `engine/src/server_listener.cpp` (parser path) +
`engine/src/tah_burst_orchestrator.cpp` (emission path).

**Current behavior:** after the canned burst is sent, server emits
no per-tick traffic. TAH stalls.

**Target behavior:**

1. **Parse incoming C→S DataPackets** and walk the ESS for any
   SimConsoleEvent. Detect `dataFinished` as the cmd of arg 0.
2. On `dataFinished` receipt, transition the session into "steady
   state". Begin per-tick emission at `1000 / cur_delay` Hz
   (~15 Hz for cur_delay=66).
3. Each tick, emit a small (40..80 B) DataPacket with:
   - `R0=0, R1=0` (no rate change).
   - `ESS=0` unless there's a reliable event to send (chat, score
     change, etc.).
   - `PSC=1` with a small fixed payload (43 bits of zeros suffices
     for "no Control-Object delta this tick").
   - `GSS=1` with the loop terminator (single 0 bit for
     "no ghost objects to update") followed by the scope-always-
     complete bit set to 1.
4. The total packet body is then `4 (VC) + 1 byte (substream flags
   + PSC payload + GSS terminator + complete bit + padding)` ≈ 6
   bytes — but TAH actually expects a meaningful PSC, so use
   ≥ 10 B by including the captured pkt 140 PSC payload bytes.

**Complexity: M.** Adds an ESS-walker on the server (small) + a
per-tick emitter (small, like the existing
`kFirstGhostBurstTemplate` but emitted on a 66 ms timer).

**Cross-references:** §2.6, §3.3, §5, §7.8, §7.10.

### P4 — ESS-walker — parse all C→S events server-side

**File:** `engine/src/server_listener.cpp`.

**Current behavior:** unrecognised C→S DataPacket shapes are treated
as "non-movecmd DataPacket" and just touch the session.

**Target behavior:** for every C→S DataPacket of ptype=0, walk the
ESS using the §2 event-class decoders. Specifically recognise:

- `SimConsoleEvent` (wire 8): drive session state machine (set
  `client_ready=true` on `SetCLInfo`, set `data_finished=true` on
  `dataFinished`).
- `PlayerSayEvent` (wire 83): echo to other connected sessions.

For unrecognised event classes, bail out of the walk (don't try to
read body of unknown class). Bodies the server doesn't need to act
on can be skipped if their class is known but the schema isn't
worth implementing (DeltaScoreEvent, etc.).

**Complexity: M.** Bulk of the work is the Huffman string decoder
(see §1.5 — copy the 256-entry CHAR_FREQS table verbatim, build the
tree by pop-up-two-smallest construction).

**Cross-references:** §1.5, §2.

### P5 — Ping reply handler (ptype=7)

**File:** `engine/src/server_listener.cpp`.

**Current behavior:** ptype=7 packets are dropped.

**Target behavior:** on receipt of a ptype=7 packet, emit a 4-byte
pure-ack response (see §6.4).

**Complexity: S.** A single conditional in the packet-dispatch path.

**Cross-references:** §6.4.

### P6 — Stop emitting all-at-once burst; pace by cur_delay

**File:** `engine/src/server_listener.cpp` + `tah_burst_orchestrator.cpp`.

**Current behavior:** `build_initial_burst()` returns N packets all
emitted back-to-back at the AC-accept tick.

**Target behavior:** emit one burst packet per `cur_delay` ms
(66 ms for TAH default). After each emitted packet, schedule the
next emit `cur_delay` ms later. Use the existing
`last_outbound_ms` field on `Session` to track timing.

**Complexity: M.** A per-session timer + a queue of pre-built
packets to drain.

**Cross-references:** §1.4 (sliding window — when queue exceeds
32 unacked packets, stall), §4.33, §7.7.

### Quick wins / cleanup

- Remove the `kFirstGhostBurstTemplate` fallback (P3 makes it
  irrelevant for vanilla too; emit a proper Phase 1 + catalogue for
  all sessions).
- Drop the `kTahAcceptConnectTemplate[18]` constants (already
  unused).

---

## §9 — Open questions / UNRESOLVED

### §9.1 — GSS substream body schemas

The Ghost SubStream is the largest remaining gap. `rev20` skips it
entirely; the Frohnmayer paper documents the algorithm without bit-
level layout; the pcap shows ~2000 GSS-bearing packets but their
per-ghost bodies (especially the variable-width class-tag at create
and the per-class state-mask-driven body at update) are not
characterised.

The `TRIBES-GHOST-CLASSES.md` clean-room spec has a partial
class-tag enumeration derived from hex inspection of cap1. The
`CAP1-GHOST-INTRO-DECODE.md` §0 finding stands: per-class GSS body
schemas need a separate Reader pass that consults a different
corpus (e.g. live captures from a debuggable client) than the
ones available here.

**For TAH-load-screen progression this is not a blocker** because
the load-screen transition triggers on the first PSC-bearing packet,
not on receipt of a populated GSS. An empty GSS (loop terminator
immediately) is valid.

### §9.2 — Per-class PSC body for the player Control-Object

The Move-Manager S→C Control-Object state is ~43 bits per tick in
the pcap (pkt 140). The exact field-by-field layout (position quantum,
orientation, animation index, ...) is not in `rev20` or the paper.

Workaround: replay the captured 43-bit PSC payload on every tick.
TAH will accept the bit-count but see no motion.

### §9.3 — Per-class GSS body for the ghosted player avatar

Once gameplay starts, the player's own avatar appears as a ghost on
their own client (because the simulation is symmetric — the Control
Object is "ghosted" to its own viewer). The bit layout of the player
ghost's update body is not specified.

This is a follow-up spec; not blocking the load screen.

### §9.4 — The "burn data" segments inside catalogue bodies

Many per-type catalogue bodies (StaticData, VehicleData, ProjectileData,
etc.) contain large opaque "burn data" bit-runs (96 to 632 bits) that
`rev20` skips without decoding. These encode the underlying
gameplay constants (max velocity, jet thrust, weapon damage, etc.).

For TAH-progression these can be all-zeros — TAH's catalogue
unpacker reads them by bit-count, not by value. Implementer should
emit the correct bit-count per §4 schemas and zero contents.

The actual per-byte field semantics are outside this spec.

### §9.5 — Reconnect-time AC variants

The pcap contains AC packets at t=56.197 s and t=106.517 s with
`byte 1 = 0x70` (highest-recv-seq nonzero) — these are reconnects
on an existing VC. For first-connect (the load-screen path) the
existing 16 B AC formula in P0 is correct; reconnect-time encoding
of `hrcv` into byte 1 needs follow-up but does not block load.

### §9.6 — Server build identification

The public TAH server at `104.128.49.65:28006` is "T1 Pickup v1.3.1"
per the MODInfo string in pkt 5. Whether this build's wire protocol
deviates from stock TAH (1998 Sierra build) is not characterised.
Most likely no — the wire bytes match the rev20-documented format
exactly — but for completeness, captures against the stock Sierra
TAH would close any "is this a TAH-mod-specific quirk" worry.

### §9.7 — wire-ptype=12 vs rev20-ptype=3 for AC

The wire-level packet-type field at bit 27..31 reads `12` for AC; the
`rev20/T1Connection.h:PACKET_CONNECTION_ACCEPT` is `3`. This 4×
multiplier suggests the wire encoding shifts the rev20-enum by 2
bits (or the field has a 2-bit prefix this spec hasn't characterised).
This doesn't block anything — the existing AC encoder in
`server_listener.cpp` emits the right 16 bytes — but tracking it
down would help reconcile the rev20 enum with the live wire.

---

## Appendix A — Reader notes (audit trail)

Sources consulted by this Reader:

- **Frohnmayer paper:** `docs/clean-room-specs/TribesEngineNetworkingModel.txt`.
- **Netcode-rev20 corpus:**
  - `docs/clean-room-specs/netcode-rev20/PROVENANCE.md`
  - `docs/clean-room-specs/netcode-rev20/t1events.txt`
  - `docs/clean-room-specs/netcode-rev20/t1demoformat.txt`
  - `docs/clean-room-specs/netcode-rev20/t1eventsother.txt`
  - `docs/clean-room-specs/netcode-rev20/t1structure.txt`
  - `docs/clean-room-specs/netcode-rev20/Info.txt`
  - `docs/clean-room-specs/netcode-rev20/Howto.txt`
  - `docs/clean-room-specs/netcode-rev20/T1NetStream.{cpp,h}`
  - `docs/clean-room-specs/netcode-rev20/T1EventManager.{cpp,h}`
  - `docs/clean-room-specs/netcode-rev20/bitStream.{cpp,h}`
- **Live pcap:** `tribes-game/captures/tah-to-public-2026-06-16.pcap`
  (decoded with custom Python tooling under `/tmp/full-protocol-analysis/`:
  `parse_pcap.py`, `bitreader.py`, `huffman.py`, `full_walk.py`).
- **Prior clean-room specs (used for reference, not re-derived):**
  `TRIBES-NETPROTO.md`, `TRIBES-DATABLOCKS.md`,
  `TRIBES-PROTOCOL-PCAP-DIFF.md`, `TAH-REQUESTCONNECT-SHAPES.md`,
  `CAP1-GHOST-INTRO-DECODE.md`, `TRIBES-CATALOGUE-BODIES.md`,
  `TRIBES-INITIAL-BURST.md`, `TRIBES-GHOST-CLASSES.md`,
  `TRIBES-PHASE2-PACKING.md`.

The Reader used the Frohnmayer paper as the architectural spine,
the rev20 text documents (`t1events.txt`, `t1demoformat.txt`) as the
authoritative per-event byte-schema reference, the rev20 source as
behaviour reference (paraphrased only, no verbatim code in the spec
body), and the live pcap as ground-truth disambiguator on all
disagreements.

Forbidden references explicitly NOT consulted during preparation:
`github.com/MortarTurret/Darkstar`, `github.com/MortarTurret/extinction`,
`github.com/sdozeman/starsiege-tribes`,
`github.com/BaseEncrypt/TotalAnnihilation`,
`github.com/kingtomato/tribes.tools`.

---

## Appendix B — Implementer firewall

**The Implementer agent working from this spec MUST NOT directly
consult any of the following:**

- The `netcode-rev20` source code (`T1*.{cpp,h}`, `bitStream.{cpp,h}`,
  `main.cpp`, `Console.{cpp,h}`).
- The rev20 text documentation (`t1events.txt`, `t1demoformat.txt`,
  `t1eventsother.txt`, `t1structure.txt`, `Info.txt`, `Howto.txt`).
- The Frohnmayer paper (the architectural model is fully recapped in
  §1 here; no need to re-read).
- Any leaked Dynamix engine mirror.

**The Implementer MAY:**

- Use the live pcap (`tah-to-public-2026-06-16.pcap`) for ground-truth
  verification of byte-level encodings.
- Use the byte tables in this spec verbatim.
- Use the Huffman char-freq table in §1.5 (a constant data array).
- Write new tooling that parses the pcap (LSB-first bit reader is
  trivial; see §1.5 for byte ordering).
- Consult the open-source `TheKigen/t1net-go` repository for the
  server-browser protocol (out-of-band discovery only; not relevant
  to the in-band content protocol covered here).

**The Implementer's clean-room status is preserved as long as no
field name, function name, type name, or algorithmic structure is
borrowed from leaked source.** Names in this spec (e.g. `db_type`,
`group_size`, `wire_tag`) are this Reader's own and are
NON-canonical — the Implementer may pick different names freely.

This spec IS the firewall between the leaked / community-clean-room
sources and the Implementer's eventual code.
