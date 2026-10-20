# Z-Wave R2 (908.40 MHz, 40 kbps, ±20 kHz FSK) on the Heltec V3 SX1262, raw GFSK.
import time, struct
import heltec

XTAL = 32_000_000
def freg(hz): return int(hz * (1 << 25) // XTAL)

def setup(m, freq_hz=908_400_000, sync=b"\x55\x55", plen=64, dbm=-6):
    m._buf_view = memoryview(bytearray(16))  # FSK packet params need 10 bytes
    m.standby()
    m._cmd("BB", 0x8A, 0x00)                                   # packet type GFSK
    m._cmd(">BI", 0x86, freg(freq_hz))                         # RF frequency
    br = 32 * XTAL // 40000                                    # 40 kbps
    fd = freg(20000)                                           # ±20 kHz
    m._cmd(">BBBBBBBBB", 0x8B, (br >> 16) & 0xFF, (br >> 8) & 0xFF, br & 0xFF,
           0x00, 0x19, (fd >> 16) & 0xFF, (fd >> 8) & 0xFF, fd & 0xFF)   # no shaping, RxBw 0x19 = 156 kHz (0x1A was 78 kHz: too narrow)
    # preamble 80 bits, detector 16 bits, sync len bits, no addr, FIXED length, CRC off, no whitening
    m._cmd(">BHBBBBBBB", 0x8C, 80, 0x05, 8 * len(sync), 0x00, 0x00, plen, 0x01, 0x00)
    for i, b in enumerate(sync):
        m._reg_write(0x06C0 + i, b)
    m._reg_write(0x08AC, 0x96)                                 # RX gain: boosted
    m._cmd("BBB", 0x8F, 0x00, 0x00)                            # buffer base
    m._cmd(">BHHHH", 0x08, 0x0003, 0x0003, 0, 0)               # IRQ TxDone|RxDone on DIO1
    m._cmd("BBBBB", 0x95, 0x04, 0x07, 0x00, 0x01)              # PA config (SX1262 HP)
    m._cmd("BbB", 0x8E, dbm, 0x04)                             # TX power, 200 us ramp

def irq(m):
    r = m._cmd(">BB", 0x12, 0, n_read=2); return (r[0] << 8) | r[1]

def clr(m): m._cmd(">BH", 0x02, 0xFFFF)

def listen(seconds=20, plen=64, sync=b"\x55\x55"):
    m = heltec.radio(freq_khz=908400)
    setup(m, sync=sync, plen=plen)
    m._cmd(">BHHHH", 0x08, 0x03FF, 0x03FF, 0, 0)               # every IRQ source, so every flag gets cleared
    clr(m)
    m._cmd(">BBH", 0x82, 0xFF, 0xFFFF)                         # RX continuous
    end = time.ticks_add(time.ticks_ms(), seconds * 1000)
    n = 0
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        f = irq(m)
        if not f:
            continue
        clr(m)
        if f & 0x0002:
            st = m._cmd("BB", 0x13, 0, n_read=2); ln, ptr = st[0], st[1]
            buf = bytearray(ln)
            m._cmd("BBB", 0x1E, ptr, 0, read_buf=buf)
            ps = m._cmd("BB", 0x14, 0, n_read=3)
            n += 1
            print("RX t=%d rssi_sync=%.1f rssi_avg=%.1f %s" % (time.ticks_ms(), -ps[1] / 2, -ps[2] / 2, bytes(buf).hex()))
        if (m._cmd("BB", 0xC0, 0, n_read=1)[0] >> 4) & 7 != 5:      # fell out of RX: re-arm
            m._cmd(">BBH", 0x82, 0xFF, 0xFFFF)
    m.standby()
    print("done, packets:", n)

def send(frame_hex, count=5, gap_ms=300, dbm=-6, inv=True):
    m = heltec.radio(freq_khz=908400)
    fr = bytes.fromhex(frame_hex)
    cs = 0xFF
    for x in fr: cs ^= x
    fr = fr + bytes([cs])
    if inv:                                                    # real Z-Wave: logical 1 = lower tone
        fr = bytes(b ^ 0xFF for b in fr)
    setup(m, sync=b"\xAA\x0F" if inv else b"\x55\xF0", plen=len(fr), dbm=dbm)
    for i in range(count):
        m._cmd("BB", 0x8F, 0, 0)
        m._cmd("BB", 0x0E, 0, write_buf=fr)
        clr(m)
        m._cmd(">BBH", 0x83, 0, 0)                             # TX, no timeout
        t = time.ticks_ms()
        while not irq(m) & 0x0001 and time.ticks_diff(time.ticks_ms(), t) < 200: pass
        print("sent", i, fr.hex())
        time.sleep_ms(gap_ms)
    m.standby()
