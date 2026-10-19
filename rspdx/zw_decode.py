#!/usr/bin/env python3
"""Decode Z-Wave R2 (40 kbit/s, ±20 kHz FSK) frames from a zw_chan.py 908 channel.

Symbol lock: at h = 1 the two tones are orthogonal over one bit, so per bit compare
the energy at centre+20 kHz with centre-20 kHz (non-coherent FSK), choosing the
integration start that maximises the mean contrast |U-D|/(U+D). The centre is
read per burst from the clipped FM discriminator (between its 20th and 80th
percentiles). Frames: preamble 0x55.., SOF 0xF0, XOR checksum (init 0xFF). Both
bit polarities are tried; real Z-Wave decodes with inv=1 (logical 1 = lower tone).

usage: zw_decode.py CHANNEL_908.npy [--fs 800e3] [--thr-db 4] [--bits]
"""
import argparse
from collections import Counter

import numpy as np
from scipy.signal import firwin, filtfilt

ap = argparse.ArgumentParser()
ap.add_argument("channel")
ap.add_argument("--fs", type=float, default=800e3)
ap.add_argument("--thr-db", type=float, default=4.0, help="burst threshold over the median floor")
ap.add_argument("--bits", action="store_true", help="print the first 400 bits of each burst")
a = ap.parse_args()

FS2, RATE = a.fs, 40e3
SPS = FS2 / RATE
y = np.load(a.channel)
KIND = {1: "singlecast", 2: "multicast", 3: "ACK", 8: "routed"}

env = np.convolve(np.abs(y) ** 2, np.ones(80) / 80, "same")
floor = np.median(env)
on = np.where(env > floor * 10 ** (a.thr_db / 10))[0]
segs = []
if on.size:
    st = last = on[0]
    for i in on[1:]:
        if i - last > 400:
            segs.append((st, last)); st = i
        last = i
    segs.append((st, last))

h = firwin(65, 45e3 / (FS2 / 2))


def bits_of(a0, b0):
    x = y[max(a0 - 40, 0): b0 + 40]
    s = filtfilt(h, 1, x)
    d = np.clip(np.angle(s[1:] * np.conj(s[:-1])) * FS2 / 2 / np.pi, -40e3, 40e3)
    c = (np.percentile(d, 20) + np.percentile(d, 80)) / 2
    n = np.arange(len(x))
    up = x * np.exp(-2j * np.pi * (c + 20e3) / FS2 * n)
    dn = x * np.exp(-2j * np.pi * (c - 20e3) / FS2 * n)
    best = None
    for ph in range(int(SPS)):
        k = int((len(x) - ph) / SPS)
        U = np.abs(up[ph: ph + k * int(SPS)].reshape(k, -1).sum(1)) ** 2
        D = np.abs(dn[ph: ph + k * int(SPS)].reshape(k, -1).sum(1)) ** 2
        q = np.mean(np.abs(U - D) / (U + D + 1e-9))
        if best is None or q > best[0]:
            best = (q, U > D)
    return best[1].astype(np.uint8), c, best[0]


def checksum_ok(fr):
    cs = 0xFF
    for v in fr[:-1]:
        cs ^= int(v)
    return cs == fr[-1]


for a0, b0 in segs:
    dur = (b0 - a0) / FS2 * 1e3
    if dur < 0.5:
        continue
    bits, c, eye = bits_of(a0, b0)
    out = f"t={a0/FS2:8.4f}s {dur:7.2f} ms centre {c/1e3:+5.1f} kHz contrast {eye:4.2f}"
    hits = []
    for inv in (0, 1):
        bb = bits ^ inv
        s = "".join(map(str, bb))
        i = 0
        while True:
            i = s.find("0101010111110000", i)
            if i < 0:
                break
            j = i + 16
            raw = np.packbits(bb[j: j + (len(bb) - j) // 8 * 8])
            if len(raw) >= 2 and raw[0] == 0x55:
                hits.append(f"inv={inv} BEAM node={raw[1]}")
                i = j + 24
                continue
            if len(raw) >= 10:
                L = int(raw[7])
                if 10 <= L <= len(raw) and checksum_ok(raw[:L]):
                    fr = raw[:L]
                    fc = int(fr[5]) << 8 | int(fr[6])
                    hits.append(f"inv={inv} FRAME home={fr[:4].tobytes().hex()} src={fr[4]} dst={fr[8]} "
                                f"{KIND.get(fc >> 8 & 0xF, 'h%d' % (fc >> 8 & 0xF))} ackreq={bool(fc & 0x4000)} "
                                f"seq={fc & 0xF} payload={fr[9:-1].tobytes().hex()}")
                    i = j + 8 * L
                    continue
            i = j
    beams = [x for x in hits if "BEAM" in x]
    if beams:
        out += f"\n   {len(beams)} beam fragments, most common: {Counter(beams).most_common(2)}"
    for x in [x for x in hits if "BEAM" not in x][:6]:
        out += "\n   " + x
    if a.bits:
        out += "\n   " + "".join(map(str, bits[:400]))
    print(out, flush=True)
