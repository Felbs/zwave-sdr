#!/usr/bin/env python3
"""Cut the Z-Wave channels out of a zw_rec.py capture.

usage: zw_chan.py CAPTURE.npy PREFIX [--fs 8e6] [--fc 912.2e6]
Writes PREFIX_908.npy and PREFIX_916.npy: complex64 at fs/10 (800 kS/s), centred on
908.40 / 916.00 MHz, ±150 kHz. Polyphase decimation (scipy upfirdn) so only the
output samples are computed -- a plain convolve at 8 MS/s took >10 min per 45 s.
"""
import argparse

import numpy as np
from scipy.signal import upfirdn

ap = argparse.ArgumentParser()
ap.add_argument("capture")
ap.add_argument("prefix")
ap.add_argument("--fs", type=float, default=8e6)
ap.add_argument("--fc", type=float, default=912.2e6)
a = ap.parse_args()

FS, FC, DEC = a.fs, a.fc, 10
CH = {"908": 908.40e6, "916": 916.00e6}
iq = np.load(a.capture, mmap_mode="r")
taps = 255
h = (np.sinc(2 * 150e3 / FS * (np.arange(taps) - taps // 2)) * np.blackman(taps)).astype(np.float32)
h /= h.sum()
CH_N = int(FS)                                  # 1 s chunks
nall = len(iq) // 2
out = {k: [] for k in CH}
LO = {}
for c0 in range(0, nall, CH_N):
    seg = iq[2 * c0: 2 * min(c0 + CH_N + taps, nall)].astype(np.float32)
    x = (seg[0::2] + 1j * seg[1::2]).astype(np.complex64)
    for k, f0 in CH.items():
        w = -2 * np.pi * (f0 - FC) / FS
        if k not in LO:
            LO[k] = np.exp(1j * w * np.arange(CH_N + taps)).astype(np.complex64)
        lo = LO[k][: len(x)] * np.complex64(np.exp(1j * ((w * c0) % (2 * np.pi))))
        y = upfirdn(h, x * lo, down=DEC)[taps // DEC: taps // DEC + CH_N // DEC]
        out[k].append(y.astype(np.complex64))
for k in CH:
    y = np.concatenate(out[k])
    np.save(f"{a.prefix}_{k}.npy", y)
    p = np.abs(y[: len(y) // 8000 * 8000]).reshape(-1, 8000) ** 2
    pk = 10 * np.log10(p.max(1) + 1e-9)
    fl = np.median(10 * np.log10(np.median(p, 1) + 1e-9))
    hot = np.where(pk > fl + 15)[0]
    print(f"{k}: {len(hot)} of {len(pk)} 10-ms slots have a burst >15 dB over the floor")
