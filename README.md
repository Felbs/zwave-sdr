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
| `sx1302/` | `zwctl1302.c` a small hub: add a device, read/set a thermostat setpoint, wake-up beam, `cmd` for any command class · `zwsurvey.py` decodes the replies (setpoints, modes, sensors, battery, configuration parameters) · `zwtx1302.c` transmit · `zwack1302.c` receive and auto-ACK · `libloragw-sx1302-raw-fsk.patch` | a host with an SX1302/SX1250 concentrator (tested: RAK5146 over USB in an ex-FreedomFi box) |
| `heltec/` | `zwheltec.py` raw-FSK receive/send for MicroPython on a Heltec WiFi LoRa 32 V3 · `zwdev.py` a mock device for testing the hub's add sequence | the board, via mpremote or a raw-REPL helper |

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

## The SX1302 (RAK5146) as a Z-Wave radio — with ACKs

Semtech's newer concentrator does the same job with register-level framing:
the patch adds `lgw_fsk_raw` to `sx1302_send` (fixed length, no CRC, no
whitening, no Gaussian shaping, no length byte). Two things that cost time:

- **The transmitter reuses the receiver's FSK sync word.** Receive on
  `0x5555` and your ACKs go out with `5555` where the start-of-frame belongs.
  Set the FSK channel's sync word to `0xAA0F` — the Z-Wave SOF in the chip's
  native (inverted) polarity — for both directions; received blocks then
  start at the Home ID.
- The FSK receiver is IF chain 9, and the TX gain table rejects an SX1250
  entry unless `mix_gain` is 5–15 even though the SX1250 ignores it.

Measured (Heltec sends frames that ask for an ACK, `zwack1302` answers, an SDR
records both): 6/6 ACKs correct, **4.3–5.1 ms of silence** after the frame,
so the ACK's start-of-frame arrives ~6.7 ms after the frame ends — a little
earlier than the real battery thermostat's (~7.2 ms), well behind a
mains-powered hub (~2.5 ms). The host's share: the frame is fetched over USB
1.1–1.9 ms after the concentrator timestamps it; transmit start-up is ~3.5 ms.

```
gcc ... zwack1302.c -lloragw -ltinymt32 -lparson -lbase64 -lrt -lm -lpthread
./zwack1302 30 2          # answer frames addressed to node 2 for 30 s
ZW_PWRIDX=3 ./zwtx1302 10 300 c0dec0de0141010b0200
```

## A small hub: adding a device, reading and setting a setpoint

`zwctl1302` keeps its own network ID (`home_id`, random on first run, first two
bits set as the protocol reserves for hubs) and is node 1. The sequence it
runs when its owner puts a device in "add" mode — verified on the air with an
SDR recording every frame, against the mock device in `heltec/zwdev.py`:

```
device (node 0, temporary ID)  →  broadcast   node-information frame     01 01 ...
hub → device (temporary ID)                    assign IDs                 01 03 <node> <home ID×4>   (ACK)
hub → node 2 (our ID)                          request node information   01 02                      (ACK)
node 2 → hub                                   node-information frame     01 01 ...                  (ACK)
hub → node 2   thermostat setpoint get   43 02 01      →   report 43 03 01 2A 02 A8  = 68.0 °F
hub → node 2   thermostat setpoint set   43 01 01 2A 02 CB (71.5 °F)       (ACK)   → get reads 71.5
```

The protocol command numbers and layouts are from the Z-Wave JS source
(protocol CC 0x01; NIF 0x01, request 0x02, assign IDs 0x03 with the node ID
first then the four Home ID bytes; three capability bytes then basic, generic,
[specific] device class and the command-class list). The *order* of steps is
the ordinary one every open hub implements, written from memory of the public
protocol description and not yet confirmed against a commercial device: the
real thermostat is still on its original hub. Not implemented: neighbour
discovery, S2 security (the device will join without it), removing a device.

A mock written by the same person proves self-consistency, not conformance;
the SDR recording proves what was on the air. The next test is a real
thermostat, whose own buttons start the add sequence.

`zwctl1302 beam NODE HASH` sends the 1.11 s wake-up beam we measured
(`[preamble, SOF, 0x55, node, Home-ID hash]` repeated) for a sleeping battery
device. One measured sample is not enough to pin the hash function (94
CRC-8 variants fit it), so the hash will be found by trial on our own device.

## The Heltec as the referee

`zwheltec.py` drives the SX1262 in raw GFSK with the same grid: 40 kbit/s,
±20 kHz, no shaping, 156 kHz RX bandwidth, fixed length, CRC and whitening
off. `listen()` prints every block that follows `0x5555`; `send()` transmits
a frame with the checksum appended. Why it exists: the SDR turned out to be a
poor judge of the SX1301's transmissions (a few dB above the floor on the
antennas to hand), and proving transmission needs a receiver that was not
written by the same person on the same afternoon. The micropython-lib driver's
shared command buffer is 9 bytes; the FSK packet-params command needs 10, so
the script enlarges it. Receive bandwidth: code 0x1A is 78 kHz (we assumed
156) and cut the edges of a ±20 kHz, 40 kbit/s signal a few kHz off centre —
frames came and went at random until it was set to 0x19 (156 kHz).

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
- [x] SX1302 transmit + automatic ACK, on-air ACK timing comparable to a real battery device
- [ ] Explain the SX1301 board's weak transmit (~39 dB below an SX1262 at the same setting, same room; the chip's power steps are linear)
- [x] Heltec receive reliability — it was the RX bandwidth code (0x1A is 78 kHz, not 156; 0x19 is), plus RX gain boost: 15/15
- [x] Add a device, read and set a thermostat setpoint — against a mock device, every frame confirmed on the air by an SDR
- [x] The same with the real thermostat, on its wall: paired as node 2, setpoint read and set both ways, mode switched, a configuration parameter changed, and unsolicited reports received after setting the lifeline association
- [x] Wake-up code found by trial (`scan`: 3 tries); the hash function itself is still unknown (two samples now)
- [ ] S2 security, removing a device, listening continuously while the concentrator also serves LoRa

## What the real thermostat taught us (that the mock could not)

Three things stood between "works against a mock" and "works against the
thermostat", all found with the SDR recording what the thermostat actually
sent while its owner pressed *include*:

1. **Its announcement is 50 bytes** (20 command classes); the hub's fixed
   receive block was 32.
2. **It announces inside a hub-search envelope** (frame type 5, 12 bytes
   before the node-information frame), not as a plain broadcast.
3. **Its preamble is ~170 bits long.** The SX1302's FSK modem waits only 128
   bit-times after detecting a preamble for the sync word to arrive
   (`FSK_TIMEOUT`, HAL default 128), then gives up — so the concentrator
   heard the alarm hub and the mock (short preambles) and never the
   thermostat, even from a metre away. Raised to 1023. If a chip hears
   some senders and not others, compare preamble lengths against its sync
   timeout before blaming signal level.

Also: the over-the-air node-information frame has **no basic-device-class
byte** (capabilities ×3, generic, specific, command classes), unlike the
serial-API form; the thermostat answers a wake-up beam + command within
~31 ms; back-to-back beams lose replies, so leave a few seconds between
commands; and the lifeline association (group 1 → node 1) makes it report
setpoint and mode changes on its own.

MIT. Measurements and captures from a suburban house, one commercial alarm
hub, one Z-Wave Plus thermostat.
