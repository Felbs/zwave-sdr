#!/usr/bin/env python3
"""Find Z-Wave R2 frames in zwrx raw blocks (stdin or file): either polarity, any bit offset."""
import sys

KIND = {1: "singlecast", 2: "multicast", 3: "ACK", 8: "routed"}

def bits_of(b):
    return [(x >> (7 - i)) & 1 for x in b for i in range(8)]

def bytes_of(bits):
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits) - 7, 8))

def frames(raw):
    out = []
    for inv in (0, 1):
        bb = [x ^ inv for x in bits_of(raw)]
        s = "".join(map(str, bb))
        i = s.find("0101010111110000")
        if i < 0:
            continue
        fr = bytes_of(bb[i + 16:])
        if len(fr) >= 2 and fr[0] == 0x55:
            out.append(f"inv={inv} BEAM node={fr[1]}")
            continue
        if len(fr) < 10:
            continue
        L = fr[7]
        if 10 <= L <= len(fr):
            cs = 0xFF
            for x in fr[:L - 1]:
                cs ^= x
            ok = cs == fr[L - 1]
            fc = fr[5] << 8 | fr[6]
            out.append(f"inv={inv} {'OK ' if ok else 'BAD'} home={fr[:4].hex()} {fr[4]}->{fr[8]} "
                       f"{KIND.get(fc >> 8 & 0xF, 'h%d' % (fc >> 8 & 0xF))} seq={fc & 0xF} payload={fr[9:L-1].hex()}")
        else:
            out.append(f"inv={inv} sof but len={L}: {fr[:12].hex()}")
    return out

src = open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin
for line in src:
    if "size=" not in line:
        continue
    parts = line.split()
    raw = bytes.fromhex(parts[-1])
    res = frames(raw)
    head = " ".join(parts[:4])
    print(head, " | ".join(res) if res else "(no SOF) " + parts[-1][:40], flush=True)
