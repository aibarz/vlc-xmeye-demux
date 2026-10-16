#!/usr/bin/env python3
# XMEye proprietary stream -> standard media container
# Based on the frame layout documented by XMEye NetSDK and the MIT project
# https://github.com/polesse/xmeye-dvr-utilities

import argparse, mmap, os, statistics, struct, subprocess, tempfile
from pathlib import Path

MARKER = b'\x00\x00\x01'

def u16(b): return struct.unpack('<H', b)[0]
def u32(b): return struct.unpack('<I', b)[0]

def parse(src: Path, tmpdir: Path):
    frames=[]; fps_samples=[]; have_key=False; current_ts=None; ts_start=None; since_ts=0
    with src.open('rb') as fh:
        data=mmap.mmap(fh.fileno(),0,access=mmap.ACCESS_READ)
        pos=0
        while pos < len(data)-4:
            idx=data.find(MARKER,pos)
            if idx<0 or idx+3>=len(data): break
            t=data[idx+3]
            try:
                if t==0xF9:  # timestamp
                    if idx+24>len(data): break
                    if have_key:
                        ts=u32(data[idx+8:idx+12]); current_ts=ts
                        if ts_start is not None and ts>ts_start and since_ts>0:
                            fps_samples.append(since_ts/(ts-ts_start))
                        ts_start=ts; since_ts=0
                    pos=idx+24
                elif t==0xFC:  # I frame, 16-byte private header
                    if idx+16>len(data): break
                    have_key=True
                    ts=u32(data[idx+8:idx+12]); current_ts=ts
                    if ts_start is not None and ts>ts_start and since_ts>0:
                        fps_samples.append(since_ts/(ts-ts_start))
                    ts_start=ts; since_ts=0
                    n=u32(data[idx+12:idx+16])
                    if idx+16+n>len(data): break
                    frames.append(('V',idx+16,n,current_ts)); since_ts+=1; pos=idx+16+n
                elif t==0xFD:  # P frame, 8-byte private header
                    if idx+8>len(data): break
                    n=u32(data[idx+4:idx+8])
                    if idx+8+n>len(data): break
                    if have_key:
                        frames.append(('V',idx+8,n,current_ts)); since_ts+=1
                    pos=idx+8+n
                elif t==0xFA:  # G.711 A-law audio
                    if idx+8>len(data): break
                    n=u16(data[idx+6:idx+8])
                    if idx+8+n>len(data): break
                    if have_key: frames.append(('A',idx+8,n,current_ts))
                    pos=idx+8+n
                else:
                    pos=idx+4
            except Exception:
                pos=idx+4

        if not frames: raise RuntimeError('No XMEye frames found')
        fps=statistics.median(fps_samples) if fps_samples else 25.0
        if not (0.5 <= fps <= 120): fps=25.0
        vpath=tmpdir/'video.h264'; apath=tmpdir/'audio.alaw'
        vc=ac=0
        with vpath.open('wb') as vf, apath.open('wb') as af:
            for kind,start,n,_ in frames:
                if kind=='V': vf.write(data[start:start+n]); vc+=1
                else: af.write(data[start:start+n]); ac+=1
        data.close()
    head=vpath.read_bytes()[:256]
    codec='hevc' if head.startswith(bytes([0,0,0,1,0x40])) or head.startswith(bytes([0,0,1,0x40])) else 'h264'
    return fps,vpath,apath,vc,ac,codec

def convert(src: Path, out: Path):
    with tempfile.TemporaryDirectory(prefix='xmeye_', dir=r'D:\Temp') as td:
        fps,v,a,vc,ac,codec=parse(src,Path(td))
        cmd=['ffmpeg','-y','-loglevel','error','-f',codec,'-framerate',f'{fps:.6f}','-i',str(v)]
        if ac and a.stat().st_size:
            cmd += ['-f','alaw','-ar','8000','-ac','1','-i',str(a)]
        cmd += ['-map','0:v:0']
        if ac and a.stat().st_size: cmd += ['-map','1:a:0?']
        cmd += ['-c:v','copy','-aspect','16:9']
        if ac and a.stat().st_size: cmd += ['-c:a','aac','-b:a','64k']
        cmd += [str(out)]
        subprocess.run(cmd,check=True)
        print(f'OK codec={codec} fps={fps:.3f} video_frames={vc} audio_frames={ac} output={out}')

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('input'); p.add_argument('-o','--output')
    a=p.parse_args(); src=Path(a.input); out=Path(a.output) if a.output else src.with_suffix('.mkv')
    convert(src,out)


