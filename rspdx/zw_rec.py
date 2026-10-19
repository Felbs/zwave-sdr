#!/usr/bin/env python3
"""Record raw I/Q covering both US Z-Wave channels (908.4 and 916.0 MHz) into RAM, then save.

No processing while recording: a weak CPU that filters live drops samples (we measured
45 % kept with a live burst detector on a Celeron J1900, 99.9 % recording straight to RAM).
The samples/wall-clock ratio is printed; anything under 0.98 means the capture is void.

usage: zw_rec.py SECONDS OUT.npy [--antenna "Antenna A"] [--fs 8e6] [--fc 912.2e6] [--ifgr 35] [--rfgr 2]
Output: interleaved int16 I/Q (.npy). Needs SoapySDR (written for an SDRplay RSPdx).
"""
import argparse
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CS16

ap = argparse.ArgumentParser()
ap.add_argument("seconds", type=float)
ap.add_argument("out")
ap.add_argument("--antenna", default="Antenna A")
ap.add_argument("--fs", type=float, default=8e6)
ap.add_argument("--fc", type=float, default=912.2e6)
ap.add_argument("--ifgr", type=int, default=35, help="SDRplay IF gain reduction, dB")
ap.add_argument("--rfgr", type=int, default=2, help="SDRplay LNA state")
a = ap.parse_args()

N = int(a.seconds * a.fs)
need = N * 4
avail = int(open("/proc/meminfo").read().split("MemAvailable:")[1].split()[0]) * 1024
if need > avail - 600e6:
    sys.exit(f"refusing: need {need/1e9:.2f} GB, MemAvailable {avail/1e9:.2f} GB")
buf = np.zeros(2 * N, np.int16)

dev = SoapySDR.Device(SoapySDR.Device.enumerate({"driver": "sdrplay"})[0])
dev.writeSetting("biasT_ctrl", "false")
assert dev.readSetting("biasT_ctrl") == "false"      # never power an antenna by accident
dev.setAntenna(SOAPY_SDR_RX, 0, a.antenna)
dev.setSampleRate(SOAPY_SDR_RX, 0, a.fs)
dev.setBandwidth(SOAPY_SDR_RX, 0, 8e6)
dev.setGainMode(SOAPY_SDR_RX, 0, False)
dev.setGain(SOAPY_SDR_RX, 0, "IFGR", a.ifgr)
dev.setGain(SOAPY_SDR_RX, 0, "RFGR", a.rfgr)
dev.setFrequency(SOAPY_SDR_RX, 0, a.fc)
st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16)
dev.activateStream(st)
n = 0
t0 = time.time()
print("RECORDING", flush=True)
while n < N:
    sr = dev.readStream(st, [buf[2 * n:]], min(65536, N - n), timeoutUs=500000)
    if sr.ret > 0:
        n += sr.ret
wall = time.time() - t0
dev.deactivateStream(st)
dev.closeStream(st)
ratio = N / (wall * a.fs)
print(f"samples/wall = {ratio:.4f} ({wall:.2f}s)", "INTEGRITY", "OK" if ratio > 0.98 else "VOID")
np.save(a.out, buf)
