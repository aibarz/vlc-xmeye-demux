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

## Validated integration status — 2026-10-17

The current Windows integration is considered **validated** on VLC 3.0.24 x64 after real-device playback tests.

Validated paths:

- Native XMEye `.h264` wrapper demux through `libxmeye_plugin.dll`.
- XMEye AVI exports that advertise H.264 while carrying HEVC, through the patched `libavi_plugin.dll`.
- Mid-GOP `Local_*.h264` exports whose first FC/VPS/SPS/PPS appears beyond the initial 256 KiB probe window.
- Native frame-rate detection from XMEye timing records, including observed 8 fps and 25 fps streams.

Playback was verified on a more capable ASUS system with VLC 3.0.24 x64: NVR AVI, NVR native XMEye recordings, and `Local_*` HEVC recordings all displayed correctly without grey-block corruption. The HP T520 remains performance-limited for the heavier 2592x1944/25 fps HEVC `Local_*` streams; this is treated as a hardware throughput limitation rather than a demux-format failure.

The validated deployment baseline is the paired demux set:

- `plugins\demux\libxmeye_plugin.dll`
- patched `plugins\demux\libavi_plugin.dll`
