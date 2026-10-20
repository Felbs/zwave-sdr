#!/usr/bin/env python3
"""Decode the replies from `zwctl1302 cmd` (reads its output on stdin)."""
import sys, re
MODES={0:"off",1:"heat",2:"cool",3:"auto",4:"aux heat",5:"resume",6:"fan only",7:"furnace",8:"dry",9:"moist",10:"auto changeover",11:"energy heat",12:"energy cool",13:"away",15:"full power",31:"manufacturer"}
OPST={0:"idle",1:"heating",2:"cooling",3:"fan only",4:"pending heat",5:"pending cool",6:"vent/economizer",7:"aux heating",8:"2nd stage heating",9:"2nd stage cooling",10:"2nd stage aux heat",11:"3rd stage aux heat"}
FAN={0:"auto low",1:"low",2:"auto high",3:"high",4:"auto medium",5:"medium",6:"circulation",7:"humidity circ",8:"left-right",9:"up-down",10:"quiet",11:"external circ"}
FANST={0:"idle/off",1:"running",2:"running high",3:"running medium",4:"circulation",5:"humidity circ",6:"right-left circ",7:"up-down circ",8:"quiet circ"}
SPT={1:"heating",2:"cooling",7:"furnace",8:"dry air",9:"moist air",10:"auto changeover",11:"energy save heat",12:"energy save cool",13:"away heat",14:"away cool",15:"full power"}
SENS={1:"temperature",5:"humidity"}
def val(b):
    prec=b[0]>>5; scale=(b[0]>>3)&3; size=b[0]&7
    v=int.from_bytes(bytes(b[1:1+size]),"big",signed=True)
    return v/(10**prec), scale, 1+size
def bits(bb, names):
    out=[]
    for i,byte in enumerate(bb):
        for k in range(8):
            if byte>>k&1: out.append(names.get(i*8+k, "#%d"%(i*8+k)))
    return out
for line in sys.stdin:
    m=re.search(r"payload=([0-9a-f]*)", line)
    if line.startswith("send"): print(line.strip()); continue
    if not m or "reply" not in line: continue
    p=bytes.fromhex(m.group(1)); cc,cm=p[0],p[1]; r=p[2:]
    if cc==0x43 and cm==3: v,sc,_=val(r[1:]); print(f"   setpoint {SPT.get(r[0]&0xF, r[0])}: {v} {'F' if sc else 'C'}")
    elif cc==0x43 and cm==5: print("   setpoints supported:", bits(r, SPT))
    elif cc==0x40 and cm==3: print("   mode:", MODES.get(r[0]&0x1F, r[0]))
    elif cc==0x40 and cm==5: print("   modes supported:", bits(r, MODES))
    elif cc==0x42 and cm==3: print("   operating state:", OPST.get(r[0]&0xF, r[0]))
    elif cc==0x44 and cm==3: print("   fan mode:", FAN.get(r[0]&0xF, r[0]), "(off)" if r[0]&0x80 else "")
    elif cc==0x44 and cm==5: print("   fan modes supported:", bits(r, FAN))
    elif cc==0x45 and cm==3: print("   fan state:", FANST.get(r[0]&0xF, r[0]))
    elif cc==0x31 and cm==5: v,sc,_=val(r[1:]); print(f"   sensor {SENS.get(r[0], r[0])}: {v} {('F' if sc else 'C') if r[0]==1 else '%'}")
    elif cc==0x31 and cm==2: print("   sensors supported:", bits(r, SENS))
    elif cc==0x80 and cm==3: print("   battery:", "LOW" if r[0]==0xFF else f"{r[0]} %")
    elif cc==0x72 and cm==5: print(f"   manufacturer id {r[0]:02x}{r[1]:02x}, product type {r[2]:02x}{r[3]:02x}, product id {r[4]:02x}{r[5]:02x}")
    elif cc==0x86 and cm==0x12: print(f"   library {r[0]}, protocol {r[1]}.{r[2]}, firmware {r[3]}.{r[4]}" + (f", hw {r[5]}" if len(r)>5 else ""))
    elif cc==0x81 and cm==6: print(f"   clock: day {r[0]>>5} {r[0]&0x1F:02d}:{r[1]:02d}")
    elif cc==0x70 and cm==6: n=r[1]&7; v=int.from_bytes(bytes(r[2:2+n]),"big",signed=True); print(f"   parameter {r[0]} = {v}")
    elif cc==0x5e and cm==2: print(f"   Z-Wave Plus v{r[0]}, role {r[1]}, node type {r[2]}, installer icon {r[3]:02x}{r[4]:02x}, user icon {r[5]:02x}{r[6]:02x}")
    elif cc==0x85 and cm==6: print(f"   association groups: {r[0]}")
    elif cc==0x85 and cm==3: print(f"   association group {r[0]} (max {r[1]}) nodes: {list(r[3:])}")
    else: print("   reply", p.hex())
