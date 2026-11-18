"""btsnoop_hci.log -> BLE-MIDI SysEx 메시지 목록(양방향, 시간순).

  python3 -I scripts/btsnoop_sysex.py <btsnoop_hci.log>
"""
import struct,sys,collections
f=open(sys.argv[1],'rb').read(); off=16
pkts=[]
while off+24<=len(f):
    olen,ilen,flags,drops,ts=struct.unpack('>IIIIq',f[off:off+24]); d=f[off+24:off+24+ilen]; off+=24+ilen
    pkts.append((ts,flags,d))
# reassemble ACL -> L2CAP
reasm={}; att=[]
for ts,flags,d in pkts:
    if not d or d[0]!=2: continue
    h,l=struct.unpack('<HH',d[1:5]); handle=h&0xfff; pb=(h>>12)&3; p=d[5:5+l]
    sent= (flags&1)==0
    key=(handle,sent)
    if pb in (0,2):
        if len(p)<4: continue
        ln,cid=struct.unpack('<HH',p[:4]); reasm[key]=[ln,cid,bytearray(p[4:]),ts]
    else:
        if key in reasm: reasm[key][2]+=p
        else: continue
    ln,cid,buf,t0=reasm[key]
    if len(buf)>=ln:
        if cid==4: att.append((t0,sent,handle,bytes(buf[:ln])))
        del reasm[key]
# ATT: writes (0x52 cmd, 0x12 req) from phone, notifications 0x1b from piano
opc=collections.Counter((a[1],a[3][0]) for a in att)
print('ATT opcodes (sent,op):',dict(opc))
def midi_payload(v):
    # BLE-MIDI: header byte, then [timestamp] msgs; strip bytes with bit7 set that are timestamps
    out=bytearray(); i=1
    while i<len(v):
        b=v[i]
        if b&0x80 and i+1<len(v) and (v[i+1]&0x80):  # timestamp followed by status
            i+=1; continue
        if b&0x80 and b not in (0xF0,0xF7) and b<0xF8 and out and out[-1]!=0xF7 and 0xF0 in out[-20:] and False: pass
        out.append(b); i+=1
    return bytes(out)
streams={True:bytearray(),False:bytearray()}; events=[]
for t,sent,h,a in att:
    op=a[0]
    if op in (0x52,0x12) and sent: v=a[3:]
    elif op==0x1b and not sent: v=a[3:]
    else: continue
    if len(v)<2 or not (v[0]&0x80): continue
    events.append((t,sent,v))
import re
def sysex_msgs(evs):
    msgs=[]; cur={True:None,False:None}
    for t,sent,v in evs:
        i=1
        while i<len(v):
            b=v[i]
            if b&0x80:
                # timestamp byte precedes status bytes; F0/F7 always preceded by ts in BLE-MIDI except continuation
                if i+1<len(v) and v[i+1]&0x80 and b not in (0xF0,0xF7):
                    i+=1; continue
                if b==0xF0: cur[sent]=bytearray([0xF0])
                elif b==0xF7:
                    if cur[sent] is not None: cur[sent].append(0xF7); msgs.append((t,sent,bytes(cur[sent]))); cur[sent]=None
                elif cur[sent] is not None and b>=0xF8: pass
                else:
                    # timestamp byte inside sysex continuation (before F7) -> skip
                    pass
            else:
                if cur[sent] is not None: cur[sent].append(b)
            i+=1
    return msgs
ms=sysex_msgs(events)
t0=ms[0][0] if ms else 0
for t,sent,m in ms:
    print(f"{(t-t0)/1e6:9.3f} {'PH->PI' if sent else 'PI->PH'} {m.hex(' ')}")
