#!/usr/bin/env python3
"""V2: send acks every 250ms with correct 3-bit run-length encoding
(1..7 per run, 0 = terminator)."""
import socket, time, os

HOST, PORT = "127.0.0.1", 28001
OUT_DIR = "/tmp/tah_full_burst_v2"
os.makedirs(OUT_DIR, exist_ok=True)
for f in os.listdir(OUT_DIR): os.remove(f"{OUT_DIR}/{f}")

RC = bytes.fromhex(
    "05002141 6181a1c1 e101 18"
    "cafeba"
    "0801000d 56c6bb6f 09c4321a 1140046981931e3c1378 3be5e30b 508e1737 c40918f8 5b64df657b"
)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))

def write_bits(buf, cursor, value, n):
    for i in range(n):
        if (cursor + i) // 8 >= len(buf):
            buf.extend([0] * ((cursor + i) // 8 - len(buf) + 1))
        buf[(cursor + i) // 8] |= ((value >> i) & 1) << ((cursor + i) % 8)
    return cursor + n

def encode_ack(parity, highest_5bit, runs):
    """runs: list of (start_5bit, length) with length in 1..7."""
    buf = bytearray()
    bc = 0
    bc = write_bits(buf, bc, 1, 1)
    bc = write_bits(buf, bc, 1 if parity else 0, 1)
    bc = write_bits(buf, bc, 1, 9)
    bc = write_bits(buf, bc, highest_5bit & 0x1F, 5)
    for (start, length) in runs:
        assert 1 <= length <= 7, f"length must be 1..7, got {length}"
        bc = write_bits(buf, bc, length, 3)
        bc = write_bits(buf, bc, start & 0x1F, 5)
    bc = write_bits(buf, bc, 0, 3)  # terminator
    bc = write_bits(buf, bc, 16, 5)  # type Ack
    while bc % 8 != 0: bc = write_bits(buf, bc, 0, 1)
    return bytes(buf)

def build_runs_from_seqs(sorted_seqs):
    """Collapse contiguous seqs into (start, length) runs of <=7."""
    runs = []
    i = 0
    while i < len(sorted_seqs):
        start = sorted_seqs[i]
        j = i
        while j + 1 < len(sorted_seqs) and sorted_seqs[j + 1] == sorted_seqs[j] + 1:
            j += 1
        full_len = j - i + 1
        for chunk_off in range(0, full_len, 7):
            runs.append((start + chunk_off, min(7, full_len - chunk_off)))
        i = j + 1
    return runs

# Phase 1
s.sendto(RC, (HOST, PORT))
s.settimeout(2.0)
ac, _ = s.recvfrom(2048)
print(f"AC: {ac.hex()}")
parity = (ac[0] & 0x02) != 0

# Phase 2: initial ack of seq=1
s.sendto(encode_ack(parity, 1, [(1, 1)]), (HOST, PORT))

# Phase 3: collect for 8 seconds. Send updated ack every 250ms.
s.setblocking(False)
seen = {}  # seq -> payload
end = time.time() + 8.0
last_ack_send = time.time()
last_new_seq = time.time()
while time.time() < end:
    try:
        pkt, _ = s.recvfrom(4096)
        b0 = pkt[0]; b1 = pkt[1] if len(pkt) > 1 else 0
        seq = ((b0 >> 2) & 0x3F) | ((b1 & 0x07) << 6)
        if seq not in seen:
            seen[seq] = pkt
            last_new_seq = time.time()
            print(f"  new seq={seq:3d}  {len(pkt):3d}B  {pkt[:10].hex()}...")
    except BlockingIOError:
        time.sleep(0.005)
    if time.time() - last_ack_send > 0.25:
        sorted_seqs = sorted(seen.keys()) if seen else [1]
        if 1 not in seen:
            sorted_seqs = [1] + sorted_seqs
        runs = build_runs_from_seqs(sorted_seqs)[:7]
        highest = max(sorted_seqs) & 0x1F
        s.sendto(encode_ack(parity, highest, runs), (HOST, PORT))
        last_ack_send = time.time()
    # Exit early if server's been quiet for >2s
    if time.time() - last_new_seq > 2.0:
        break

print(f"\n[summary] {len(seen)} unique seqs: {sorted(seen.keys())}")
total_b = 0
for seq in sorted(seen.keys()):
    p = seen[seq]; total_b += len(p)
    with open(f"{OUT_DIR}/seq{seq:03d}_{len(p):03d}B.bin", "wb") as f:
        f.write(p)
print(f"saved {total_b}B to {OUT_DIR}/")
