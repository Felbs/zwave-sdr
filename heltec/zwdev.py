# Mock Z-Wave device on the Heltec V3 for testing a hub's "add device" sequence.
# Acts like a thermostat whose owner pressed "add": announces itself (NIF, node 0, temporary
# Home ID), accepts AssignIDs, answers RequestNIF and Thermostat Setpoint Get/Set.
import time, os
import heltec, zwheltec

HOME = bytearray(b"\xDE\xAD" + os.urandom(2))
NODE = 0
HUB = 1
SEQ = 1
SETPOINT = 680          # tenths of a degree F
NIF = bytes([0x01, 0x01, 0x93, 0x04, 0x00, 0x04, 0x08, 0x06, 0x43, 0x40, 0x80, 0x86])   # listening, 40k, specific present; thermostat; CCs

def frame(home, src, dst, ftype, ackreq, seq, payload):
    fr = bytes(home) + bytes([src, (0x40 if ackreq else 0) | ftype, seq & 0x0F, 10 + len(payload), dst]) + payload
    cs = 0xFF
    for b in fr: cs ^= b
    return fr + bytes([cs])

def tx(m, fr):
    zwheltec.setup(m, sync=b"\xAA\x0F", plen=len(fr), dbm=-6)
    m._cmd("BB", 0x8F, 0, 0)
    m._cmd("BB", 0x0E, 0, write_buf=bytes(b ^ 0xFF for b in fr))
    zwheltec.clr(m)
    m._cmd(">BBH", 0x83, 0, 0)
    t = time.ticks_ms()
    while not zwheltec.irq(m) & 0x0001 and time.ticks_diff(time.ticks_ms(), t) < 100: pass
    rx_on(m)

def rx_on(m):
    zwheltec.setup(m, sync=b"\xAA\x0F", plen=32, dbm=-6)
    m._cmd(">BHHHH", 0x08, 0x03FF, 0x03FF, 0, 0)
    zwheltec.clr(m)
    m._cmd(">BBH", 0x82, 0xFF, 0xFFFF)

def rx(m):
    f = zwheltec.irq(m)
    if not f: return None
    zwheltec.clr(m)
    if not f & 0x0002:
        if (m._cmd("BB", 0xC0, 0, n_read=1)[0] >> 4) & 7 != 5: m._cmd(">BBH", 0x82, 0xFF, 0xFFFF)
        return None
    st = m._cmd("BB", 0x13, 0, n_read=2)
    buf = bytearray(st[0]); m._cmd("BBB", 0x1E, st[1], 0, read_buf=buf)
    b = bytes(x ^ 0xFF for x in buf)
    L = b[7]
    if L < 10 or L > len(b): return None
    cs = 0xFF
    for x in b[:L-1]: cs ^= x
    if cs != b[L-1]: return None
    return dict(home=b[:4], src=b[4], type=b[5] & 0x0F, ackreq=bool(b[5] & 0x40), seq=b[6] & 0x0F, dst=b[8], payload=b[9:L-1])

def learn(seconds=40):
    global HOME, NODE, SEQ, SETPOINT
    m = heltec.radio(freq_khz=908400)
    rx_on(m)
    print("device: temp home %s node %d, announcing" % (HOME.hex(), NODE))
    end = time.ticks_add(time.ticks_ms(), seconds * 1000)
    last_nif = 0; joined = False
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if not joined and time.ticks_diff(time.ticks_ms(), last_nif) > 1000:
            tx(m, frame(HOME, NODE, 0xFF, 1, False, SEQ, NIF)); SEQ += 1; last_nif = time.ticks_ms()
            print("sent NIF")
        f = rx(m)
        if not f: continue
        if f["home"] != bytes(HOME) or f["dst"] != NODE: continue
        p = f["payload"]
        if f["ackreq"] and f["type"] == 1:
            tx(m, frame(HOME, NODE, f["src"], 3, False, f["seq"], b""))
        if p[:2] == b"\x01\x03" and len(p) >= 7:               # AssignIDs
            NODE = p[2]; HOME = bytearray(p[3:7]); joined = True
            print("joined: home %s node %d" % (HOME.hex(), NODE))
        elif p[:2] == b"\x01\x02":                             # RequestNIF
            time.sleep_ms(20); tx(m, frame(HOME, NODE, f["src"], 1, True, SEQ, NIF)); SEQ += 1
            print("sent NIF to hub")
        elif p[:2] == b"\x43\x02":                             # Setpoint Get
            time.sleep_ms(20)
            tx(m, frame(HOME, NODE, f["src"], 1, True, SEQ, bytes([0x43, 0x03, p[2], 0x2A, SETPOINT >> 8, SETPOINT & 0xFF]))); SEQ += 1
            print("sent setpoint report %d" % SETPOINT)
        elif p[:2] == b"\x43\x01" and len(p) >= 6:             # Setpoint Set
            SETPOINT = (p[4] << 8) | p[5]
            print("setpoint now %d" % SETPOINT)
    m.standby()
    print("device done")
