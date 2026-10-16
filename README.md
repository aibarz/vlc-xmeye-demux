# vlc-xmeye-demux

Native VLC demux plugin for XMEye proprietary DVR/NVR recordings.

## Goal

Open XMEye recordings directly in VLC without transcoding and without creating temporary media files. The demuxer strips the XMEye private frame headers in memory and forwards the original elementary stream payload to VLC's normal decoders/packetizers.

Current known frame markers:

- `00 00 01 FC` - video I/key frame, 16-byte private header
- `00 00 01 FD` - video P/inter frame, 8-byte private header
- `00 00 01 F9` - timestamp/control record, 24-byte record
- `00 00 01 FA` - G.711 A-law audio, 8-byte private header

The container layer and payload codec detection are deliberately separate so additional XMEye variants can be added later.

## Current known payloads

- HEVC/H.265 confirmed on real XMEye samples.
- H.264 detection planned/supported by the architecture.
- G.711 A-law audio marker identified.
- Additional proprietary variants: pending samples.

## Design

`XMEye file -> probe -> XMEye frame parser -> payload detector -> VLC ES output -> VLC decoder`

No video re-encoding is required.

## Safety

The demuxer must claim a stream only after validating multiple XMEye records and a plausible video payload. A normal `.h264`/`.h265` elementary stream must continue to be handled by VLC's standard demuxers.

## Target

Initial compatibility target: VLC 3.0.24 x64 on Windows.

See `docs/FORMAT.md`, `docs/VLC-INTEGRATION.md`, and `STATUS.md`.
