# zwave-sdr

Tools for receiving and sending Z-Wave R2 frames (908.4 MHz, 40 kbit/s FSK)
with equipment you may already have: an SDR for listening, and the LoRa
concentrator card inside a retired Helium hotspot (Semtech SX1301) for both
listening and transmitting. A Heltec LoRa board (SX1262) serves as an
independent check of what the SX1301 sends.

The goal is a home-built controller for the owner's own Z-Wave devices — the
first target is a battery thermostat — on hardware that was headed for the
recycling bin. What's here is the radio layer: the frame comes out of the air
byte-exact, and a frame typed in goes into the air byte-exact. Pairing,
routing and the S2 security layer are not implemented (yet).

The measurements behind these tools are written up in the
[Radio Grid Atlas](https://github.com/Felbs/radio-grid-atlas/tree/master/zwave-908)
entry; the hotspot-conversion side is in
[helium-upcycle](https://github.com/Felbs/helium-upcycle).

## What's measured, in one paragraph

Z-Wave's 40 kbit/s mode is two-tone FSK with ±20 kHz deviation, so the two
tones are exactly one bit-rate apart (modulation index 1) and orthogonal over
a bit. The reliable detector is therefore *which tone has the energy* per bit,
not an FM discriminator (which spikes at every transition at h = 1 unless its
filter is tuned just right). A logical 1 is the **lower** tone — the opposite
of what both LoRa-chip FSK modems do natively, so the transmit code inverts
every byte and the sync word. Frames: 0x55… preamble, SOF 0xF0, Home ID,
source, frame control, length, destination, payload, XOR checksum. A sleeping
battery device is woken by a 1.1 s "beam" of short fragments before a command.
A mains-powered controller ACKs 0.6 ms after a frame's last bit; the battery
thermostat we measured takes 3.3 ms.

## Layout

| dir | what | runs on |
|---|---|---|
| `rspdx/` | `zw_rec.py` record both US channels to RAM · `zw_chan.py` cut the 908.4/916.0 channels · `zw_decode.py` find and check frames | any Linux box with SoapySDR + numpy/scipy (written for an SDRplay RSPdx) |
| `sx1301/` | `zwrx.c` receive · `zwtx.c` transmit · `zwparse.py` frame finder/checksum · `libloragw-raw-fsk.patch` + `build.sh` | the hotspot's own CPU (tested: Nebra Outdoor, Pi CM3, Arch Linux ARM) |
| `heltec/` | `zwheltec.py` raw-FSK receive/send for MicroPython on a Heltec WiFi LoRa 32 V3 | the board, via mpremote or a raw-REPL helper |

## Listening with an SDR

```
python rspdx/zw_rec.py 45 cap.npy --antenna "Antenna A"   # prints samples/wall: must be > 0.98
python rspdx/zw_chan.py cap.npy cap                      # -> cap_908.npy, cap_916.npy
python rspdx/zw_decode.py cap_908.npy                    # frames with checksum, beams, polarity
```

Record to RAM, process later: a live burst filter at 8 MS/s on a Celeron kept
45 % of the samples. Expect frames at 908.4; 916.0 only carries the 100 kbit/s
mode, which our network never used. Make traffic on demand by changing a
setpoint from the controller's app, or pressing buttons on a battery device
(that makes the *device* transmit — it is ~6 dB weaker than a hub and may
need to be in the same room as the antenna).

## The SX1301 as a Z-Wave radio

The SX1301's FSK modem covers 500 bit/s–250 kbit/s with up to 8 sync bytes,
so Z-Wave R2 fits. What stands in the way is framing: the stock HAL
hard-codes LoRaWAN-style FSK packets (length byte, whitening, CRC). The patch
adds a global `lgw_fsk_raw` that, when set, makes `lgw_send` emit fixed-length
raw bytes; on receive the equivalent registers are written after
`lgw_start()`. Sync on `0x5555` (polarity-agnostic preamble) and let software
find the SOF.

```
cd sx1301 && ./build.sh                       # clones lora_gateway v5.0.1, patches, builds
sudo ./zwrx 60 | python3 zwparse.py           # root: the SPI device and reset GPIO need it
sudo ./zwtx 10 300 c0dec0de0141010b0200       # 10 frames, 300 ms apart, checksum appended
```

Measured on a Nebra Outdoor: 75 checksum-good frames in 50 s of a real
network, including the battery thermostat that the SDR two rooms away could
not hear; 15/15 transmitted frames received byte-exact by the Heltec at
−76 dBm in the same room. The HAL's FSK RSSI figure is not useful (it reads a
constant). Use the hotspot's own reset line before every start
(`gpioset -c gpiochip0 -t 100ms,0 38=1` on the Nebra Outdoor); the first
radio calibration fails about one start in five — just start again.

## The Heltec as the referee

`zwheltec.py` drives the SX1262 in raw GFSK with the same grid: 40 kbit/s,
±20 kHz, no shaping, 156 kHz RX bandwidth, fixed length, CRC and whitening
off. `listen()` prints every block that follows `0x5555`; `send()` transmits
a frame with the checksum appended. Why it exists: the SDR turned out to be a
poor judge of the SX1301's transmissions (a few dB above the floor on the
antennas to hand), and proving transmission needs a receiver that was not
written by the same person on the same afternoon. The micropython-lib driver's
shared command buffer is 9 bytes; the FSK packet-params command needs 10, so
the script enlarges it.

## Polarity, measured three ways

| path | decodes with |
|---|---|
| real controller → SX1301 | inverted |
| real controller → SX1262 | inverted |
| SX1301 → SX1262, bytes as given | not inverted |
| SX1301 → SX1262, bytes and sync word XOR 0xFF | inverted — same as the real network, in the same receive window |

So `zwtx` inverts by default (`ZW_NOINV=1` for the chip-native polarity).

## Privacy

A Home ID identifies a network. The decoders print it because you need it to
build a controller for your own devices; the Atlas entry masks it, captures
are git-ignored here, and none should be shared without masking.

## Status

- [x] SDR: record, channelize, decode (91 checksum-verified frames across two captures)
- [x] SX1301 receive (hears the battery device the SDR could not)
- [x] SX1301 transmit, verified byte-exact on an independent receiver
- [ ] ACK timing on the SX1301 (target: 0.6 ms like the hub; retries at 40–75 ms give some slack)
- [ ] Inclusion (pairing) with no security, then S2
- [ ] Set a thermostat setpoint from the hotspot

MIT. Measurements and captures from a suburban house, one commercial alarm
hub, one Z-Wave Plus thermostat.
