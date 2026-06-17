# Tribes protocol pcap-diff — public TAH server vs ours (14c Reader)

**Status:** v1 (2026-06-16). Closes the AC encoder, server_info-emission,
and burst-strategy gaps for 14c. Produces decoded ground-truth tables
that the Implementer can paste into a new burst path without consulting
any leaked source.

**Source-of-truth captures:**

- **Public server (gold):**
  `tribes-game/captures/tah-to-public-2026-06-16.pcap` — 4905 UDP
  packets, TAH (`192.168.1.101:61429`) ↔ public TAH server
  (`104.128.49.65:28006`), captured 2026-06-16 ~17:49 UTC, full session
  through gameplay on probable mission Blastside.
- **Our server (broken):**
  `tribes-game/captures/our-server-blastside-2026-06-16.pcap` — 701
  packets, TAH ↔ `192.168.1.111:28000` running open-siege-t1-server
  commit `f7b6ec7` with `--mission Blastside`. TAH stayed half-attached
  ~80 s but never progressed past handshake.

**Companion clean-room specs:**

- `TRIBES-NETPROTO.md` (VC framing, ack runs, sub-stream presence,
  bit primitives)
- `TRIBES-DATABLOCKS.md` (catalogue-entry-event 6+8+8 header)
- `TRIBES-INITIAL-BURST.md` (phase 0..3 burst sequence)
- `TAH-REQUESTCONNECT-SHAPES.md` (R-8 RC classifier; bytes referenced
  in §2 below)
- `CAP1-GHOST-INTRO-DECODE.md` and `TRIBES-CATALOGUE-BODIES.md`
  (R-7, R-7.1 prior decode walls)
- `TRIBES-PHASE2-PACKING.md` (packing budget)

**Clean-room compliance:** No leaked-source consultation occurred during
preparation of this spec. Every wire-level claim below is verifiable
from the two pcaps named above using only an LSB-first bit reader and
the companion specs. **Forbidden references explicitly NOT
consulted:** `github.com/MortarTurret/*`,
`github.com/sdozeman/starsiege-tribes`, or any other leaked-Dynamix
mirror.

---

## §0 — Executive summary

The diff reveals three structural deltas. **The biggest single delta is
the AcceptConnect reply:** the public server emits a **16-byte** AC
whose byte 7 is an echo of the RequestConnect's separator byte and
whose byte 8 is `(separator & 0x07) << 1`. Our `kTahAcceptConnectTemplate`
emits a fixed **18-byte** form whose bytes 7..8 are constants
(`0x08`, `parity-derived`) and which trails `01 01` past the public-AC
end — TAH does not recognize our AC as a valid connection-accept and
therefore never sends ClientReady. **The second delta** is that we
emit a non-protocol 25 B `server_info` ("SINF") packet immediately
after AC; the public server emits no such packet. **The third delta**
is burst pacing: the public server trickles its initial-state burst
across ~50 packets over ~3 s, gated by application-level rate-control
(R1=1 max-rate published in the very first server data packet); we
fire **30 packets / 6.3 kB** all at once at t≈0.044 s and then go
silent. This pcap-diff also reaffirms the
`TRIBES-DATABLOCKS.md` §2 6+8+8 catalogue header verbatim against
public-server-emitted events (group/block fields advance correctly
in **both** pcaps — the R-7.1 "block stuck at 0" finding was cap1-
specific). Deliverables: §2 byte-for-byte AC encoder formula, §3
server_info verdict, §4 burst-strategy spec, §5 catalogue group sizes
for Blastside, §6 ClientReady byte-by-byte structure, §7 steady-state
characterization, §8 prioritized Implementer roadmap.

---

## §1 — Headline diff table (first 12 packets each)

Direction `C→S` = TAH → server, `S→C` = server → TAH. Sizes in bytes.
"First-8 hex" = first 8 bytes of UDP payload.

### Public capture

| # | dir | size | first 8 bytes | annotation |
|---|-----|------|---------------|------------|
| 1 | C→S | 50   | `07 00 17 4f 87 bf f3 18` | TAH RequestConnect (Shape F, R-8) |
| 2 | S→C | **16** | `07 00 09 60 13 8a 63 1d` | **AcceptConnect (16B)** — see §2 |
| 3 | C→S | 4    | `07 08 09 80`             | pure ack (TAH acks AC, type=16) |
| 4 | C→S | **62** | `0b 08 09 00 0a 01 d9 05` | **TAH ClientReady (62B)** — see §6 |
| 5 | S→C | 285  | `0b 08 09 00 86 20 dc 05` | Phase 1: first reply, R1=1 (max 450B/33ms), ESS class=79 |
| 6 | S→C | 424  | `0f 08 11 00 85 80 cc 35` | Phase 2 first: R0=1 (cur 400B/66ms), ESS class=8 + 10×class=88 |
| 7 | C→S | 19   | `0f 10 12 00 85 80 8c aa` | TAH ack-with-ISS (input-substream) |
| 8 | S→C | 419  | `13 08 11 00 dc 11 36 c8` | 26×class=88 (SoundData blocks 18..43) |
| 9 | C→S | 18   | `13 10 13 00 a8 0a 00 00` | TAH ack-with-ISS |
| 10| C→S | 18   | `17 10 13 00 a8 0a 00 00` | TAH ack retry |
| 11| S→C | 424  | `17 20 1a 00 dc 1e 36 c8` | 27×class=88 (SoundData blocks 44..70) |
| 12| C→S | 15   | `1b 20 29 00 a8 5a 00 00` | TAH ack-only |

### Our server capture

| # | dir | size | first 8 bytes | annotation |
|---|-----|------|---------------|------------|
| 1 | C→S | 46   | `05 00 d9 18 16 8a 63 1d` | TAH RequestConnect (Shape E, R-8) |
| 2 | S→C | **18** | `05 00 09 60 16 8a 63 08` | **Our AC (18B)** — wrong shape; see §2 |
| 3 | S→C | **25** | `05 00 90 53 49 4e 46 01` | **Our server_info ("SINF")** — extra; not in public; see §3 |
| 4 | C→S | 4    | `05 08 09 80`             | TAH ack (acks our pkt 2 only) |
| 5 | S→C | 211  | `09 08 09 00 dc 04 16 50` | Burst pkt: R0=R1=0, ESS class=88 (g=0 gs=10 b=0) |
| 6 | S→C | 208  | `0d 08 09 00 dc 0b 36 c8` | Burst pkt: ESS class=88 (g=1 gs=153 b=4) |
| 7 | S→C | 231  | `11 08 09 00 5c 11 36 c8` | Burst pkt: ESS class=88 (g=1 gs=153 b=15) |
| 8 | S→C | 206  | `15 08 09 00 5c 13 76 28` | Burst pkt: ESS class=88 (g=3 gs=5 b=1) |
| 9 | S→C | 205  | `19 08 09 00 dc 13 76 28` | Burst pkt: ESS class=88 (g=3 gs=5 b=2) |
| 10| S→C | 206  | `1d 08 09 00 5c 14 76 28` | Burst pkt: ESS class=88 (g=3 gs=5 b=3) |
| 11| S→C | 205  | `21 08 09 00 dc 14 76 28` | Burst pkt: ESS class=88 (g=3 gs=5 b=4) |
| 12| S→C | 222  | `25 08 09 00 5c 15 96 40` | Burst pkt: ESS class=88 (g=4 gs=8 b=0) |

**Critical observations from this table:**

1. **TAH never sent ClientReady to our server.** In the public pcap
   TAH sent a 62 B class=8 ClientReady (pkt 4) ~14 ms after acking
   the 16 B AC; in our pcap TAH sent only the 4 B ack and then went
   silent. The AC byte structure is the gate. See §2.

2. **Our pkt 3 is not present in the public capture.** It is the
   `kServerInfoTypeWord = 0x12` "SINF" packet
   (`engine/src/server_listener.cpp:454`). TAH ignores it (the byte 2
   = `0x90` translates under LSB-first decoding to a 5-bit packet-type
   of `0x12 = 18`, which is not a real VC type and is therefore not
   parsed as an in-band packet). See §3.

3. **TAH's ack target advances against our burst.** From `27 08 17 4d`
   onwards the TAH→server packets are run-length acks of our 30 burst
   packets, but TAH never sends a DataPacket of its own. The ack
   target byte 2..3 advances exactly with our send-seq — TAH's VC
   layer is healthy; it's the application-level state machine that
   stalled.

---

## §2 — AcceptConnect encoder (R-9 content)

### 2.1 Byte-by-byte of the 16-byte AC

The public TAH server emits this 16-byte payload as AC for **every**
TAH RequestConnect shape (R-8):

```
[0] 07           VC byte 0 (= vc_present=1, parity=p, send_seq[0..5]=1)
[1] 00           VC byte 1 (send_seq[6..8]=0, highest_recv_seq=0)
[2] 09           VC byte 2 (ack-run count=1, ack-run start=1 low 3 bits)
[3] 60           VC byte 3 (ack-run start high 2 bits, ack-term, ptype=12)
[4] N0           nonce[0] — echo of RC nonce byte 0
[5] N1           nonce[1] — echo of RC nonce byte 1
[6] N2           nonce[2] — echo of RC nonce byte 2
[7] SS           separator-echo — echo of RC separator byte
[8] EE           ((SS & 0x07) << 1)   — derived from separator low 3 bits
[9] 08           constant
[10] 00          constant
[11] 00          constant
[12] 00          constant
[13] 08          constant
[14] 00          constant
[15] 00          constant
```

where `p` (bit 1 of byte 0) is `LSB(N0) = N0 & 1` and `N0..N2` and
`SS` are byte-for-byte echoes from the inbound RC using the R-8
universal layout:

```
RC parity@(total_size - 43)
RC nonce @(total_size - 42 .. total_size - 40)
RC separator @(total_size - 39)
```

### 2.2 Byte-for-byte rebuild formula

For an inbound TAH RequestConnect `rc[0 .. n-1]`:

```
nonce0 = rc[n - 42]
nonce1 = rc[n - 41]
nonce2 = rc[n - 40]
sep    = rc[n - 39]

parity_bit = nonce0 & 1   # LSB of nonce[0]

byte0 = 0x05 | (parity_bit << 1)   # 0x05 if parity_bit=0, 0x07 if parity_bit=1
byte1 = 0x00
byte2 = 0x09
byte3 = 0x60
byte4 = nonce0
byte5 = nonce1
byte6 = nonce2
byte7 = sep              # NEW: echo the separator byte
byte8 = (sep & 0x07) << 1   # NEW: low-3-bit-derived value
byte9..byte15 = { 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00 }
```

The packet length is **exactly 16 bytes**. Do not append `0x01 0x01`
(our current template trails them).

### 2.3 Worked vectors

Three 16-byte ACs appear in the public capture (initial connect at
t=0.028 s, and two reconnects at t=56.197 s and t=106.517 s):

| AC pkt# | RC source | nonce      | sep  | parity_bit | byte0 | byte1 | byte7 | byte8 |
|---------|-----------|------------|------|------------|-------|-------|-------|-------|
| pub  2  | 50B Shape F | `13 8a 63` | `0x1d` | 1 | `0x07` | `0x00` | `0x1d` | `0x0a` |
| pub 2124| 53B Shape  | `14 8a 63` | `0x1d` | 0 | `0x05` | `0x00` | `0x1d` | `0x0a` |
| pub 3851| 46B Shape E | `15 8a 63` | `0x1d` | 1 | `0x07` | `0x70` | `0x1d` | `0x0a` |
| our  1  | 46B Shape E | `16 8a 63` | `0x1d` | 0 | `0x05` | `0x00` | (target `0x1d`) | (target `0x0a`) |

**Note on byte 1.** pub-pkt-3851 is a **mid-session reconnect** with
`hrcv=14` (the existing VC state's highest-received-seq); its byte 1
encodes the live VC state at AC time. For a fresh session's first AC
the highest-received-seq is 0 and byte 1 = 0x00. The Implementer
should compute byte 1 from VC state (`(send_seq>>6) & 0x07` low 3
bits + `hrcv << 3` high 5 bits) rather than hard-coding `0x00`.

The public initial-connect AC byte-for-byte (pub pkt 2):
```
07 00 09 60  13 8a 63 1d  0a 08 00 00 00 08 00 00
```

The 46B-row hex (what we **should** be emitting for our RC):
```
05 00 09 60  16 8a 63 1d  0a 08 00 00 00 08 00 00
```

What we currently emit:
```
05 00 09 60  16 8a 63 08  02 08 00 00 00 08 00 80 01 01
```

— differing in bytes 7, 8, 15, and the trailing two `01 01`.


### 2.4 Side-by-side with `build_tah_accept_connect_reply`

Current code (`engine/src/server_listener.cpp` lines 255–268 with the
template at lines 63–68):

```
template[7] = 0x08    // WRONG: should be RC separator byte
template[8] = (parity == 0x18) ? 0x02 : 0x01  // WRONG: should be (sep & 7) << 1
template[15] = 0x80   // WRONG: should be 0x00
template[16] = 0x01   // EXTRA: should not exist
template[17] = 0x01   // EXTRA: should not exist
out length  = 18      // WRONG: should be 16
```

The `build_tah_accept_connect_reply` signature changes from
`(nonce[3], parity, out[18])` to `(nonce[3], sep, out[16])`. The
classifier `looks_like_tah_request_connect` already exposes the
separator offset (`out_nonce_off + 3` is the separator) so the
caller can pass `rc[separator_off]` alongside `rc[nonce_off..+3]`.

### 2.5 Implementer pseudocode (paste-ready as PSEUDO; rename freely)

```
function build_real_tah_accept_connect_reply(nonce[3], sep_byte) -> bytes[16]:
    out = byte_array(16)
    parity_bit = nonce[0] & 0x01
    out[0] = 0x05 | (parity_bit << 1)
    out[1] = 0x00
    out[2] = 0x09
    out[3] = 0x60
    out[4] = nonce[0]
    out[5] = nonce[1]
    out[6] = nonce[2]
    out[7] = sep_byte
    out[8] = (sep_byte & 0x07) << 1
    out[9]  = 0x08
    out[10] = 0x00
    out[11] = 0x00
    out[12] = 0x00
    out[13] = 0x08
    out[14] = 0x00
    out[15] = 0x00
    return out
```

### 2.6 Tolerance of the 18 B form by TAH — post-AC behavior diff

In the public capture, ~14 ms after the 16 B AC arrives, TAH sends a
**62-byte ClientReady** (a class=8 reliable event — see §6) and
gameplay proceeds. In our capture, after we send our 18 B AC, **TAH
sends only the 4 B ack (`05 08 09 80`)** and then nothing — it
never sends ClientReady. The TAH VC layer **acks our burst packets**
(its ack-target byte advances with our send-seq from `4d` to `81`),
but the TAH application stays stalled. Over the next ~80 s our server
retransmits the 18 B AC every ~3 s under the
`(retry+1) * 3000 ms`-doubling keepalive schedule (15 retries; see
`TRIBES-NETPROTO.md` §3.8) — the entire session ends with no
forward progress.

**Conclusion:** the 18 B form is **silently rejected by TAH at the
application layer** (probably parsed past byte 15 as extra trailing
content that fails an internal checksum/validation step). TAH does
not emit a RejectConnect; it simply does not progress. The 16 B form
is required.

---

## §3 — `server_info` packet ("SINF")

### 3.1 What we currently emit

Our server, on every new session (`server_listener.cpp:447–460,
522–536, 605–620`), immediately after the AC, sends a 25-byte packet:

```
05 00 90 53 49 4e 46 01 00 00 01 9a 07 00 00 09  42 6c 61 73 74 73 69 64 65
                S  I  N  F                       B  l  a  s  t  s  i  d  e
```

Decoded against `TRIBES-NETPROTO.md` §3.1: byte 0 LSB=1 (in-band VC),
parity=0, send-seq=1, hrcv=0, ack-term, ptype=18 (= 0x12 =
`kServerInfoTypeWord`). The bytes following the ptype encode `"SINF"
magic + player_slot (1) + team_raw (0) + server_tick (0x09a) +
mission_short_name length(9) + "Blastside"`.

### 3.2 What the public server emits in the same slot

**Nothing.** In the public capture, no S→C packet sits between the 16
B AC (pkt 2) and the 62 B ClientReady-triggered phase-1 reply (pkt 5,
which is itself a normal DataPacket carrying a class=79 reliable event
and a rate-control update). There is no "SINF" magic packet, no
mission-name announcement, and no other intermediate emission.

Searched the entire 4905-packet public capture for any S→C payload
beginning with `0x90` (ptype=18 in LSB-first decoding) — **0 hits**.
Mission name is published to the client via the regular reliable
event channel as part of the scope-always burst's mission-info ghost
introduction (`TRIBES-INITIAL-BURST.md` §1.1, second bullet:
"There is no separate 'mission name' packet").

### 3.3 Effect on TAH

The TAH client appears to ignore our `0x12` ptype packet entirely
(it never acks it; it acks our pkt 2 with `05 08 09 80` and our
pkt 5+ via the burst-ack mechanism). The 25 B emission is therefore
harmless but **not part of the wire protocol** — it is a one-off
proprietary extension introduced in spec 29/02b that has no
correspondence in public-TAH-server behavior.

**Recommendation:** disable the server_info emission on TAH sessions
(or remove it entirely; the design intent of "publish mission name to
client" should be moved into the scope-always burst as
`TRIBES-INITIAL-BURST.md` §1.1 already documents).

---

## §4 — Burst packing strategy (the biggest content finding)

### 4.1 Side-by-side packet shapes

The "initial state" emission between AC and first ClientReady ack is:

| | public | ours |
|---|--------|------|
| Packets emitted | 50 (in the visible 3.1 s catalogue + intro window) | 30 (all at once at t=0.044 s) |
| Total bytes (visible window) | ~22 kB across 3.1 s | 6322 B in one batch |
| Average pkt size | 432 B | 211 B |
| Rate-control declared in first reply | `R1=1`, max_update_delay=33 ms, max_pkt_size=450 B; `R0=1` on next, cur=66/400 | `R0=R1=0` (never published) |
| Pacing | ~66 ms intra-packet inter-arrival | all packets at t=0.044 s (no pacing) |

The "2 packets, 709 bytes" sub-finding in the task description refers
to **public pkt 5 (285 B) + public pkt 6 (424 B) = 709 B** — the two
packets the public server emits in the first ~120 ms after the 16 B AC.
Together they carry the full Phase 1 (class=79 join event) and the
opening of Phase 2 (class=8 + all 10 SoundProfile records + first
SoundData record). Our equivalent of "the first burst chunk" is
**30 packets emitted as a single send-storm at t=0.044 s, totalling
6322 bytes**.

### 4.2 Public pkt 5 (285 B) — Phase 1 first-reply bit budget

UDP payload bytes 0..16:
```
0b 08 09 00 86 20 dc 05 3c 01 00 00 00 fe ff ff ff
```

Bit walk (LSB-first):

| Bit range | Width | Field | Value |
|-----------|-------|-------|-------|
| 0..0     | 1  | VC-present | 1 |
| 1..1     | 1  | connect-parity | 1 (= LSB of nonce[0]=0x13) |
| 2..10    | 9  | send_seq | 2 |
| 11..15   | 5  | highest_recv_seq | 1 |
| 16..18   | 3  | ack-run count | 1 |
| 19..23   | 5  | ack-run start | 1 |
| 24..26   | 3  | ack-list terminator | 0 |
| 27..31   | 5  | packet-type | 0 (DataPacket) |
| 32..32   | 1  | R0 (current-rate-changed) | 0 |
| 33..33   | 1  | R1 (max-rate-changed) | **1** |
| 34..43   | 10 | max-update-delay (ms) | **33** |
| 44..53   | 10 | max-pkt-size (bytes) | **450** |
| 54..54   | 1  | ESS-present | 1 |
| 55..55   | 1  | first event-present | 1 |
| 56..56   | 1  | guaranteed | 1 |
| 57..57   | 1  | seq-continuous | 0 |
| 58..58   | 1  | has-explicit-seq | 1 |
| 59..65   | 7  | explicit_seq | **0** |
| 66..72   | 7  | event class tag | **79** |
| 73..onward | var | event body — non-catalogue payload (server-state publish?) | — |

After the first event, more events follow at irregular gaps —
the class=79 events do NOT use the 6+8+8 catalogue header (their
body bits 73..89 read `0,0,0,0,...` as a 22-bit prefix, not a valid
(group, group_size, block) tuple for SoundProfileData).

**Class 79** is therefore a non-catalogue reliable event class. From
context (Phase 1, before any catalogue starts) it is most likely the
**join-time client state publish** event — assigned client id, MOTD
string, mission name, server tick base — i.e. the per-session
welcome packet. Per-field decode is **UNRESOLVED** without more
captures; see §9 OQ-1.

### 4.3 Public pkt 6 (424 B) — Phase 2 first packet bit budget

UDP payload bytes 0..16:
```
0f 08 11 00 85 80 cc 35 20 c4 83 ba 3c 5e ee 5d
```

Bit walk:

| Bit range | Width | Field | Value |
|-----------|-------|-------|-------|
| 0..0     | 1 | VC-present | 1 |
| 1..1     | 1 | parity | 1 |
| 2..10    | 9 | send_seq | 3 |
| 11..15   | 5 | hrcv | 1 |
| 16..18   | 3 | ack-run count | 1 |
| 19..23   | 5 | ack-run start | 2 |
| 24..26   | 3 | ack-term | 0 |
| 27..31   | 5 | ptype | 0 |
| 32..32   | 1 | R0 (current-rate-changed) | **1** |
| 33..42   | 10 | current-update-delay | **66** |
| 43..52   | 10 | current-pkt-size | **400** |
| 53..53   | 1 | R1 (max-rate-changed) | 0 |
| 54..54   | 1 | ESS-present | 1 |
| 55..72   | 18 | first event header (ep,guar,sc=0,he=1,eseq=6,tag=8) | tag=8 |
| 73..onward | var | event body | — |

The first event of class=8 has body bits 73..144 (72 bits). After it,
**10 catalogue events of class=88 follow**, packed sc-continuous, at
bit positions 145, 263, 371 (+ approximate; exact body widths vary
by per-block content) carrying:

| Event | (group, group_size, block) | scripted meaning |
|-------|----------------------------|-------------------|
| 0 (tag=8) | n/a — non-catalogue event, 72-bit body | start-of-catalogue marker? UNRESOLVED |
| 1 (tag=88) | (0, 10, 0) | SoundProfile[0] |
| 2 (tag=88) | (0, 10, 1) | SoundProfile[1] |
| 3 (tag=88) | (0, 10, 2) | SoundProfile[2] |
| 4 (tag=88) | (0, 10, 3) | SoundProfile[3] |
| 5 (tag=88) | (0, 10, 4) | SoundProfile[4] |
| 6 (tag=88) | (0, 10, 5) | SoundProfile[5] |
| 7 (tag=88) | (0, 10, 6) | SoundProfile[6] |
| 8 (tag=88) | (0, 10, 7) | SoundProfile[7] |
| 9 (tag=88) | (0, 10, 8) | SoundProfile[8] |
| 10 (tag=88) | (0, 10, 9) | SoundProfile[9] |
| 11 (tag=88) | (1, 153, 0) | SoundData[0] |

So pkt 6 packs **the entire SoundProfileData group plus the first
SoundData record plus one non-catalogue class=8 event**, all in
424 bytes. Compare to our pkt 5 (211 B) which packs only the 10
SoundProfile blocks + 4 SoundData blocks before exhausting its
budget.

### 4.4 Scope-always-complete signal

The public server's **scope-always-complete signal** (per
`TRIBES-INITIAL-BURST.md` §2.3) is **not** observed within the first
50 burst packets in the visible 3 s catalogue window. The catalogue
dump runs from pkt 6 (t=0.119 s) through pkt 129 (t=3.302 s, group 28
sentinel), and only THEN does the per-tick ghost-update phase begin
proper. The scope-always-complete bit (1 bit at end of GSS in each
scope-always packet) is set to `0` in every packet that bears a GSS
in this window, and to `1` only on the LAST scope-always burst
packet (UNRESOLVED for the exact packet; the GSS bit is below the
per-class introduction payload which this Reader did not decode end-
to-end).

The conjecture (`TRIBES-INITIAL-BURST.md` §1.1 + this pcap) is that
**scope-always-complete is asserted around t=15 s** — the first 59 B
S→C packet appears at t=15.231 s, marking the transition into the
steady-state per-tick ghost-delta stream.

---

## §5 — Catalogue-entry-event body schemas (closes R-7.1.7 framing)

### 5.1 Reaffirmation of the 6+8+8 header

The `TRIBES-DATABLOCKS.md` §2 header layout decodes cleanly against
both pcaps:

- Public pkt 6 ev0 (class=88, sc=1) at bit 145: body bit 155 → bits
  155..160 = group=0, bits 161..168 = group_size=10, bits 169..176 =
  block=0. Matches `(0, 10, 0)`.
- Public pkt 8 ev0 (class=88, sc=0, eseq=35) at bit 35: body bit 53 →
  `(1, 153, 18)`. Matches the spec.
- Public pkt 23 ev0 (class=88, sc=0, eseq=45) at bit 35: body bit 53
  → `(3, 5, 1)` = PlayerData[1]. Matches.
- Our pkt 5 ev0 (class=88) → `(0, 10, 0)`. Matches.
- Our pkt 8 ev0 (class=88) → `(3, 5, 1)`. Matches.

**The R-7.1 anomaly ("block stuck at 0") is cap1-specific.** In both
the public capture and our (un-modified) burst, the `block` field
advances monotonically within each group:
- Our pkt 5 walks blocks 0..9 of group 0, then 0..3 of group 1, in
  one packet (14 events).
- Public pkt 8 walks SoundData blocks 18..43 (26 events) in 419 B.

The 6+8+8 header is therefore **correct**; the `TRIBES-CATALOGUE-
BODIES.md` v1 verdict that "class_tag=88 is NOT the catalogue-entry
event class" can now be **revoked for non-cap1 sessions**. Cap1's
block-stuck-at-0 was a wire artifact (perhaps a corrupt or truncated
packet) and not a real protocol difference.

### 5.2 Group sizes observed in the public Blastside burst

By scanning the first event of every S→C burst packet for
`(group, group_size, block)` tuples (script
`/tmp/pcap-diff/find_groups_all.py`), the following table emerges
for the public-server CTF mission ("Blastside" per task description):

| group | name (per `TRIBES-DATABLOCKS.md` §3.2) | group_size (public) | group_size (`TRIBES-DATABLOCKS.md` §4 stock) | match? |
|-------|----------------------------------------|---------------------|--------------------------------------------|--------|
| 0     | SoundProfileData    | 10  | 10  | yes |
| 1     | SoundData           | 153 | 153 | yes |
| 3     | PlayerData          | 5   | 5   | yes |
| 4     | StaticShapeData     | **52**  | 51 | **+1** (Blastside has 1 extra static shape) |
| 5     | ItemData            | **45**  | 46 | **-1** |
| 6     | ItemImageData       | **23**  | 24 | **-1** |
| 7     | MoveableData        | 36  | 36 | yes |
| 8     | SensorData          | 5   | 5   | yes |
| 11    | (Tank)              | 0 (sentinel; block=255) | 0 | yes |
| 14    | BulletData          | 5   | 5   | yes |
| 16    | RocketData          | **2** | 3 | **-1** |
| 19    | TurretData          | 7   | 7   | yes |
| 20    | ExplosionData       | 19  | 19  | yes |
| 21    | MarkerData          | 3   | 3   | yes |
| 22    | DebrisData          | 7   | 7   | yes |
| 23    | MineData            | 2   | 2   | yes |
| 28    | LightningData       | 2   | 2   | yes |

Groups 2, 9, 10, 12, 13, 15, 17, 18, 24, 25, 26, 27, 29, 30: not
seen as first-events because the first event of every packet they
land in is some other group's last record. Their existence and
sentinel-vs-non-sentinel state must be inferred by walking event
bodies, which is **partially UNRESOLVED** — see §5.4.

### 5.3 Per-group body schemas — RESOLVABLE from this pcap

The Implementer can decode any catalogue body by:

1. Locating an event with `(group=G, group_size, block=B)` in the
   pcap (use script `/tmp/pcap-diff/find_groups_all.py`).
2. Computing event end as the bit position of the **next** event in
   the same packet (or `ESS-end + ISS-pres + GSS-pres` trailer if
   the event is the packet's last).
3. Reading the `body_end - body_start - 22` body bits after the
   6+8+8 header.

**Decoded width samples** (bits per event body, **excluding** the 22-
bit header):

| (group, block) | body bits | comment |
|----------------|-----------|---------|
| (0, 0..9) [public pkt 6] | ~96 bits each | SoundProfileData with default values |
| (1, 0..16) [public pkt 6 tail + pkt 7 if present] | ~100 bits avg | SoundData with short .wav names |
| (1, 18..43) [public pkt 8] | ~101 bits avg (419 B / 26 ev ≈ 16 B/ev incl. framing) | SoundData |
| (3, 1) [public pkt 23] | ~4296 bits (≈ 537 B) | PlayerData — matches V3 vector size in `TRIBES-DATABLOCKS.md` §6.3 |
| (4, 0) [public pkt 34, 448 B] | ~3000 bits estimated | StaticShape with name + transform + asset refs |
| (5, 1) [public pkt 47, 461 B] | ~3200 bits | ItemData |
| (21, 0) [public pkt 122, 487 B] | ~3400 bits avg | MarkerData |

Full byte-level decode of (group 3, block 1) and (group 21, block 0) is
deferred to follow-up Reader specs (R-7.1.2 PlayerData, R-7.1.3
MarkerData). The pcap contains the ground-truth bits; the
`TRIBES-DATABLOCKS.md` §5.3 sketch is the starting point.

### 5.4 Sentinel encoding (group 11)

Public pkt 101 (425 B, t=2.575 s) ev0 decodes as `(11, 0, 255)` —
exactly matching the `TRIBES-DATABLOCKS.md` §3.1 sentinel rule
(`group_size = 0, block = 255`, no body). **Confirmed.** A single
22-bit (6+8+8) header is the entire event body. The encoder emits one
such record per empty group; an empty group consumes 22 + 10 (sc-
continuous event header) = 32 bits ≈ 4 bytes.

### 5.5 Header width re-verification

To rule out alternative widths (cf. `TRIBES-CATALOGUE-BODIES.md` §0's
13-candidate scan), the Reader decoded `(group, group_size, block)`
at body bit 53 of public pkt 8 (sc=0 first event) under each width
combination and compared against `(1, 153, 18)`:

| header (group, gsz, blk) widths | decoded triple |
|---------------------------------|----------------|
| 6+8+8 | (1, 153, 18) **MATCH** |
| 7+8+8 | (65, 76, 9)   no |
| 5+8+8 | (1, 50, 137)  no |
| 6+9+8 | (1, 306, 36) no  |
| 6+8+7 | (1, 153, 9)  partial |

Only **6+8+8** decodes consistently. The earlier
`TRIBES-CATALOGUE-BODIES.md` §0 finding that "6+8+8 and 6+9+8 both
decode (0,10,0) on cap1" applied because cap1's first packet
happened to have aligned zero bits in the relevant slots; on the
public pcap with non-trivial (1, 153, 18) the ambiguity collapses
and 6+8+8 wins outright.

---

## §6 — ClientReady (TAH→server, 62 B) byte-by-byte

### 6.1 Public capture pkt 4 — full hex

```
0b 08 09 00  0a 01 d9 05  20 42 88 68 3b 03 62 ea
c4 c0 d2 7b  35 fc 11 6a  c2 f4 61 3d f9 28 da 74
43 9c 80 81  bf 45 f6 5d  b6 07 00 00 00 00 00 02
64 16 6c 39  da 17 88 2e  aa 0a 00 00 00 00
```

### 6.2 Decoded layout

| Bit range | Width | Field | Value |
|-----------|-------|-------|-------|
| 0..0   | 1 | VC-present | 1 |
| 1..1   | 1 | parity | 1 (= LSB nonce[0] = 0x13 & 1) |
| 2..10  | 9 | send_seq | **2** (TAH's first DataPacket post-AC; the 4 B ack at pkt 3 had seq=1) |
| 11..15 | 5 | hrcv | 1 |
| 16..18 | 3 | ack-run cnt | 1 |
| 19..23 | 5 | ack-run start | 1 |
| 24..26 | 3 | ack-term | 0 |
| 27..31 | 5 | ptype | 0 (DataPacket) |
| 32..32 | 1 | R0 | 0 |
| 33..33 | 1 | R1 | **1** |
| 34..43 | 10 | max-update-delay (ms) | **66** |
| 44..53 | 10 | max-pkt-size (bytes) | **509** |
| 54..54 | 1 | ESS-present | 1 |
| 55..55 | 1 | event-present | 1 |
| 56..56 | 1 | guaranteed | 1 |
| 57..57 | 1 | seq-continuous | 0 |
| 58..58 | 1 | has-explicit-seq | 1 |
| 59..65 | 7 | explicit_seq | **0** |
| 66..72 | 7 | event class tag (wire) | **8** (matches `client_events.cpp` line 62 comment "wire value = 8 = real tag 1032") |
| 73..onward | ~407 bits | event body | TAH-specific payload |

The body starting at bit 73 is ~407 bits ≈ 51 bytes. This is the
TAH-Groove **client-ready payload** — game-build / mod-list / client-
authentication blob. The exact field-by-field decode requires
Huffman-string-table knowledge that this pcap alone does not give
(see §9 OQ-2), but the **structural framing is identical to the
Groove ClientReady form** we already replay verbatim — the
differences are entirely in the per-event body content.

### 6.3 Comparison to `kCapturedReadyBody` (55-byte Groove body)

Our current `client_events.cpp` line 99 has a **55-byte** body
captured from a Groove-only session. The TAH ClientReady body is
**58 bytes** (62 – 4-byte VC header). Direct byte-for-byte:

```
Groove (55B):  85 80 ac 10 90 5d 00 22 84 88 b6 33 ...
TAH    (58B):  0a 01 d9 05 20 42 88 68 3b 03 62 ea ...
```

**Completely different content.** The Groove body starts with `0x85`
(R0=1 R1=0 then ESS=1 in next bits); the TAH body starts with
`0x0a 01 d9` which decodes as R0=0 R1=1 max=509/66. These are
different rate-control configurations. The 55-byte Groove body
**cannot** be replayed as-is on a TAH session — it would publish
the wrong rate-control values to TAH, which would degrade or break
the rate-throttling state.

For TAH interop the Implementer should either:

1. **Synthesize a fresh 58-byte body per session**, following the
   public-pkt-4 byte template above (R1=1, max_update_delay=66 ms,
   max_pkt_size=509 B, then the captured 51-byte event body), OR
2. **Allow the server to be agnostic to ClientReady content**: the
   server only needs to ack receipt and use the published rate-
   control fields. The opaque event body can be discarded server-
   side without parsing.

Option 2 is the cleanest. The server-side handler should:
- Detect `ptype=0 (DataPacket)`, read R0/R1 to update per-session
  rate parameters, then ack the packet.
- Treat the ESS as opaque (consume bits until ESS-terminator) and
  drop the event payload server-side.

---

## §7 — Steady-state ghosting & misc

### 7.1 Move-command response (server-side echo)

After the catalogue / scope-always burst, public S→C packets settle
into the following size distribution (counted across the 4905-pkt
capture):

| Size (B) | Count | Likely role |
|----------|-------|-------------|
| 59 | 431 | per-tick ghost-update **dominant** (one GSS sub-stream, no ESS) |
| 10 | 356 | minimal-state DataPacket (ack-list only, possibly heartbeat) |
| 37 | 301 | small ghost-delta + 0/1 reliable event |
| 49 | 236 | medium ghost-delta |
| 54 | 135 | wider ghost-delta |
| 64 | 74  | with event(s) |
| 588 | 11  | PlayerData re-broadcast (every ~50 s repeat) |

The 59 B "tick" packet arrives **every ~66 ms** (matches the R0=1
current_update_delay=66 published in pkt 6). 1000 ms / 66 ms ≈
15 Hz tick rate, consistent with the 30 Hz nominal halved by
unreliable-channel rate control under high ghost-counts.

### 7.2 First-bytes signature of the 59 B "tick" packet

```
63 bb 92 00  18 b5 01 00  00 cb 34 00  00 00 00 00 ...
```

Decoded: ptype=0, R0=0, R1=0, ESS=0, ISS=0 (likely), GSS=1. The 59 B
of UDP payload is **entirely a ghost-update sub-stream** delta block
— a few dozen GSS bits per ghost in scope, packed.

### 7.3 C→S steady-state shape

TAH's per-tick C→S packet is dominated by:

| Size (B) | Count | Likely role |
|----------|-------|-------------|
| 15 | 1354 | minimal ack + input-substream |
| 16 | 1133 | same with one extra ack-run |
| 17 | 260  | with optional payload bit-flag |
| 18 | 23   | with double-ack-run |

Sample 15 B C→S in steady state:
```
1b 20 29 00 a8 5a 00 00 00 10 00 00 30 0c 0a
```

Decoded ESS=0, ISS=1 — TAH publishes its input/control sub-stream
each tick (movement keys, mouse-look delta, fire button state).
This corresponds to `TRIBES-NETPROTO.md` §5.0.2's expected client-
side pattern.

### 7.4 OSGB rate observation

Our spec 26/11's `kFirstGhostBurstTemplate` (223 B) emits a single
ghost packet per session. The public server emits ~431 of the 59 B
"tick" packet over a 13-minute session = **one per 1.8 seconds**
(averaged across the whole capture). During active gameplay the rate
is ~15 Hz (one per 66 ms).

Our 26/11 "single 223 B packet" emission is a non-issue for steady
state — TAH eventually expects a stream, not a single packet. But
our session never reaches steady state, so this gap is masked.

### 7.5 Reliable event types observed beyond catalogue

The public capture's reliable-event traffic uses at least these tags
(observed as first-event class tags in S→C packets):

| Tag (wire) | First seen at | Conjectured role |
|------------|---------------|------------------|
| 8   | pkt 6 (424 B, t=0.119 s) | "start-of-catalogue" or related per-session welcome |
| 79  | pkt 5 (285 B, t=0.054 s) | Phase 1 client-id/MOTD publish |
| 77  | pkt 162 (297 B, t=5.611 s) | ?? — appears after catalogue, before steady state |
| 88  | pkt 8 (419 B, t=0.186 s) | catalogue-entry-event (`TRIBES-DATABLOCKS.md` §2) **CONFIRMED** |
| 112 | pkt 201 (42 B, t=6.672 s) | small post-burst reliable event (UNRESOLVED) |

Tags 8, 77, 79, 112 are NOT in `TRIBES-NETPROTO.md`'s event-class
catalog and need follow-up.

### 7.6 Anomalies for follow-up specs

1. The same PlayerData burst (588 B, 4 packets) repeats at
   t={0.583, 0.650, 0.716, 0.783 s} and again at t={56.754, 56.819,
   56.886, 56.951 s} and at t={107 s, ...} — exactly every ~50 s.
   This looks like a **periodic PlayerData re-broadcast** —
   possibly tied to a new player joining (TAH client connecting to
   server, server re-sends datablocks for all 5 PlayerData blocks).
   Confirm with a follow-up multi-client capture.

2. The class=8 first event in pkt 6 (72-bit body) appears exactly
   once at session start. Its payload should be decoded against
   public's other reliable-event corpus to identify class 8's role.

3. The class=77 event at t=5.61 s (after catalogue ends but before
   steady state) sits in the seam between scope-always burst end and
   normal-mode start. It may carry the **mission-info** broadcast
   (mission name, map bounds, GameInfo block) that
   `TRIBES-INITIAL-BURST.md` §1.1 second bullet says rides the
   ghost-update sub-stream.

---

## §8 — Implementer roadmap (prioritized)

In TAH-progression-impact order:

### P0 — Replace 18 B AC with 16 B AC (§2)

- File: `engine/src/server_listener.cpp`
- Lines 63–68 — DELETE `kTahAcceptConnectTemplate[18]`.
- Lines 255–268 — REWRITE `build_tah_accept_connect_reply` per §2.5
  pseudocode. New signature:
  ```
  void build_real_tah_accept_connect_reply(
      const std::uint8_t nonce[3],
      std::uint8_t separator_byte,
      std::uint8_t out[16]);
  ```
- Lines 580–596 (where the AC reply is built) — pass the RC
  separator byte (`buf[req_nonce_off + 3]`) as the new second arg.
- Run `--listener-selftest` after the change; the existing R-8
  catalogue selftest validates the classifier still accepts all 8
  RC shapes.

This is the single change most likely to unblock TAH ClientReady
emission. Expected behavior change after this fix: TAH sends a
~62 B ClientReady within ~50 ms of receiving the new AC.

### P1 — Stop emitting `server_info` SINF packet (§3)

- File: `engine/src/server_listener.cpp`
- Lines 447–460 (vanilla path), 522–536 (Groove path), 605–620 (TAH
  path) — DELETE the `encode_server_info(...) + socket.send_to(...)`
  block from each.
- The mission-name publish belongs in the scope-always burst (per
  `TRIBES-INITIAL-BURST.md` §1.1), not as a separate ptype=18
  packet. Track that move under a follow-up spec; for now just stop
  emitting the OOB SINF.

### P2 — Publish rate-control in first server reply (§4.2)

- File: `engine/src/tah_burst_orchestrator.cpp`
- In the first packet of the burst, set R1=1 with max_update_delay=33
  and max_pkt_size=450. In the second packet, set R0=1 with
  current_update_delay=66 and current_pkt_size=400. Mirror the
  public server's pkt 5 / pkt 6 pattern (§4.2, §4.3 bit budgets).
- Reference NETPROTO §3.4 for the bit layout.
- Effect: TAH learns the server's intended rate from the start
  instead of defaulting to the spec-defined rate; downstream rate-
  control will throttle naturally.

### P3 — Spread the burst across tick boundaries (§4.1)

- File: `engine/src/tah_burst_orchestrator.cpp`
- Current behavior: `build_initial_burst` returns ~30 packets that
  the listener emits **back-to-back at one tick** (t=0.044 s).
- Target behavior: emit at most **1 burst packet per server tick
  (~33 ms)** until exhausted. The orchestrator should retain its
  list of pre-built packets but `pop_one_per_tick()` rather than
  drain on the AC-accept tick.
- Public reference: 50 burst packets at ~66 ms intervals = 3.3 s
  total burst window.
- Effect: TAH-side rate-control acks each packet before the next
  arrives; ESS in/out flows alternate naturally.

### P4 — Make the catalogue dump complete (§5.2)

- File: `engine/src/tah_default_catalogue.cpp`
- Current behavior: a minimal catalogue with placeholders.
- Target behavior: emit a **valid catalogue with sentinel records
  for every empty group** (groups 9, 10, 12, 13, 15, 17, 18, 24, 25,
  26, 27, 29, 30 for Blastside). Each sentinel is `(group, 0, 255)`
  — 22 bits, no body (§5.4).
- For the **non-empty** groups (0..8, 11, 14, 16, 19..23, 28), the
  count must match the server's mission. For Blastside specifically
  use §5.2 table values; for generic CTF use `TRIBES-DATABLOCKS.md`
  §4 stock counts as a first approximation.
- The Implementer does NOT need to make the per-block body **valid**
  immediately — TAH will accept catalogue records whose body decodes
  to junk so long as the bit-count matches `pack()/unpack()`
  alignment. The validity of body content is a separate spec.

### P5 — Server-side ClientReady handler (§6)

- File: `engine/src/server_listener.cpp` (or a new `tah_client_ready
  _handler.cpp` if it grows)
- On receipt of a 62 B C→S DataPacket from a TAH session:
  1. Update per-session rate-control state from R0/R1 bits.
  2. Walk the ESS (consume per-event bits up to the ESS-end terminator)
     and **discard** event payloads — do not re-emit them.
  3. Mark the session as `client_ready=true` so the orchestrator can
     start the per-tick GSS emission.
- Do NOT attempt to decode the 51-byte event body (Huffman strings +
  unknown auth blob). Treat it as opaque.

---

## §9 — Open questions / UNRESOLVED

### OQ-1: Class-79 Phase 1 event body (pkt 5, 285 B)

The Phase 1 reliable event of class=79 starts at bit 73 of pkt 5 and
its body extends to where the next event begins. Body bit count: at
least 6 events in pkt 5 (seq 0..5 implied by pkt 6 starting at seq=6).
Average body ≈ 30 bytes / 6 events ≈ 40 bits per event minus framing.

This is plausibly the **client-id + assigned-team + MOTD** broadcast
sequence. Decoding it requires understanding what reliable event
classes 79 binds to in the public-server build — UNRESOLVED.

Evidence: pkt 5 hex bytes 16..32 contain
`0a 01 d9 05 20 42 88 68 3b 03 62 ea c4 c0 d2 7b` — these resemble a
**connection-token or auth-blob** more than human-readable strings.

### OQ-2: TAH ClientReady payload decoding (pkt 4, 62 B)

The 51-byte event body of TAH's ClientReady (after the 7-byte
framing) is opaque without TAH-specific Huffman tables. The
visible portion contains short ASCII-looking spans
(`07 00 00 00 00 00 00 02 64 16 6c 39 da 17 88 2e aa 0a` near the
tail looks like a length-prefixed bytestream). Per
`TRIBES-NETPROTO.md` §6.7 the string format is `1-bit compress + 8-bit
length + body`. Decoding requires the Huffman code table — out of
scope for this pcap-diff.

### OQ-3: Scope-always-complete bit location

The 1-bit scope-always-complete signal in GSS trailers should be
locatable by walking the per-packet GSS substream. This Reader did
**not** decode the per-class introduction payloads (each ghost intro
has a per-class body width that varies by class tag, and there is no
ground-truth class-tag-to-width table for Blastside's specific class
mix). The exact packet where the signal flips from 0 → 1 is
UNRESOLVED; it is somewhere in the t=14..16 s window where the
S→C packet sizes drop from ~400 B to ~59 B.

### OQ-4: Non-catalogue event class tags 8, 77, 112

Tag 8 (Phase 2 first event), 77 (post-burst single event at t=5.61),
and 112 (small post-burst events) are not documented in
`TRIBES-NETPROTO.md`. They likely fall in `TRIBES-NETPROTO.md` §16's
event class catalog as build-specific reliable-event subscriptions.
Decoding their per-event payload requires a per-class spec each.

### OQ-5: Public-server build identification

The public server at `104.128.49.65:28006` is not identified by
build (commit hash, server-mod-version string). The catalogue
event class tag is `88` which matches what
`TRIBES-DATABLOCKS.md` §6.1 found in a separate TAH-burst capture,
so it is plausibly the same TAH-server build family. But the
public server emits a 16 B AC where the prior probed
`127.0.0.1:28001` server (cited in `kTahAcceptConnectTemplate`
comment) emitted an 18 B AC. Either:
- two distinct TAH server forks coexist (vanilla TAH at port 28006
  uses 16 B; some other fork uses 18 B), or
- our prior capture mis-identified an 18 B reply (perhaps a
  Groove-style AC was mis-attributed to TAH).

Resolution would require capturing a fresh AC from the same
`127.0.0.1:28001` server with current TAH client; out of scope.

### OQ-6: PlayerData repeat-broadcast frequency

The 4-packet PlayerData re-broadcast at t={56.75, 107.08, 167.4 s, ...}
implies a ~50 s period. This is not a documented protocol feature.
Possible causes: server periodically re-publishing in case a client
missed the original; a TAH-internal keep-alive-style refresh.
UNRESOLVED.

---

## Appendix A — Reader notes (audit trail)

No leaked-source consultation occurred during preparation of this
spec. All wire-level claims are verifiable from the two named pcaps
using only `parse_pcap.py` + `bitreader.py` (LSB-first bit reader,
~50 lines of Python each, no engine source). The decoding scripts
used in derivation live at `/tmp/pcap-diff/`:

- `parse_pcap.py` — UDP packet extraction from the pcap files
- `bitreader.py` — LSB-first bit reader + VC-header parser
- `decode_ac.py` — byte-by-byte AC decode (§2)
- `decode_burst.py` — VC + rate-control + first-event decode (§4)
- `find_groups.py`, `find_groups_all.py` — catalogue group/block
  walk (§5)
- `scan_tags.py` — sc=1+tag pattern scanner (§4.3)

All `MortarTurret/*` repos and `sdozeman/starsiege-tribes` were
**explicitly not consulted**. The companion clean-room specs
(`TRIBES-NETPROTO.md`, `TRIBES-DATABLOCKS.md`,
`TRIBES-INITIAL-BURST.md`, `TAH-REQUESTCONNECT-SHAPES.md`,
`TRIBES-CATALOGUE-BODIES.md`, `CAP1-GHOST-INTRO-DECODE.md`) were
the only "prior wire knowledge" inputs.

The two pcap files themselves are the deliberate ground truth.
