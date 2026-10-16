#!/usr/bin/env python3
"""Read-only structural probe for XMEye recordings. No extraction or transcoding."""
from __future__ import annotations
import argparse, mmap, struct
from collections import Counter
from pathlib import Path

PREFIX=b'\x00\x00\x01'

def u16(b): return struct.unpack('<H', b)[0]
def u32(b): return struct.unpack('<I', b)[0]

def codec(payload: bytes) -> str:
    h264=hevc=0
    i=0
    while i+6 < len(payload) and i < 512:
        if payload[i:i+4] == b'\x00\x00\x00\x01': sc=4
        elif payload[i:i+3] == b'\x00\x01': sc=3
        else:
            i += 1; continue
        b=payload[i+sc]
        if not b & 0x80:
            ht=b & 0x1f
            et=(b >> 1) & 0x3f
            if ht in (5,7,8): h264 += 1
            if 16 <= et <= 23: hevc += 1
            if et in (32,33,34): hevc += 2
        i += sc+1
    if hevc >= 2 and hevc > h264: return 'HEVC'
    if h264 >= 2: return 'H264'
    return 'unknown'

def probe(path: Path):
    counts=Counter(); codecs=Counter(); invalid=0; first=[]
    with path.open('rb') as fh:
        data=mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        n=len(data); pos=0
        while pos < n-16:
            idx=data.find(PREFIX,pos)
            if idx < 0 or idx+4 > n: break
            t=data[idx+3]
            if t==0xFC and idx+16<=n:
                size=u32(data[idx+12:idx+16]); header=16
            elif t==0xFD and idx+8<=n:
                size=u32(data[idx+4:idx+8]); header=8
            elif t==0xFA and idx+8<=n:
                size=u16(data[idx+6:idx+8]); header=8
            elif t==0xF9 and idx+24<=n:
                size=0; header=24
            else:
                pos=idx+4; continue
            if size > 64*1024*1024 or idx+header+size > n:
                invalid += 1; pos=idx+4; continue
            name={0xFC:'FC',0xFD:'FD',0xFA:'FA',0xF9:'F9'}[t]
            counts[name]+=1
            if len(first)<8: first.append((idx,name,size))
            if t in (0xFC,0xFD) and size:
                c=codec(data[idx+header:idx+header+min(size,512)])
                codecs[c]+=1
            pos=idx+header+size
        data.close()
    return counts,codecs,invalid,first

if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('files',nargs='+'); a=ap.parse_args()
    for f in a.files:
        p=Path(f); counts,codecs,invalid,first=probe(p)
        print(f'FILE={p}')
        print(f'SIZE={p.stat().st_size}')
        print('RECORDS='+' '.join(f'{k}:{v}' for k,v in sorted(counts.items())))
        print('CODECS='+' '.join(f'{k}:{v}' for k,v in codecs.most_common()))
        print(f'INVALID={invalid}')
        print('FIRST='+';'.join(f'{off}:{typ}:{size}' for off,typ,size in first))
        print()
