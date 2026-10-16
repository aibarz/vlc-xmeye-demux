# XMEye container notes

This document records the format observed in XMEye DVR/NVR exports. It is reverse-engineered from owned recordings and cross-checked against existing open-source XMEye tooling.

## Record prefix

All known private records begin with the three-byte prefix:

```
00 00 01
```

The fourth byte identifies the record type.

| Type | Marker | Total private header/record | Payload length |
|---|---|---:|---|
| Key/I video | `FC` | 16 bytes before payload | LE32 at file offset `record+12` |
| P/inter video | `FD` | 8 bytes before payload | LE32 at `record+4` |
| Audio | `FA` | 8 bytes before payload | LE16 at `record+6` |
| Timestamp/control | `F9` | 24 bytes total | no media payload |

## FC key frame

Observed layout:

```
00 00 01 FC  ....  timestamp_le32  payload_size_le32  <video payload>
^                                                   ^
record start                                        +16
```

The current reference parser reads the timestamp at `+8`, payload size at `+12`, then copies `size` bytes starting at `+16`.

## FD inter frame

```
00 00 01 FD  payload_size_le32  <video payload>
^                                  ^
record start                       +8
```

## FA audio

Known samples identify this as G.711 A-law. Payload size is the LE16 at `+6`; payload begins at `+8`.

## F9 timestamp/control

Total record length observed: 24 bytes. A LE32 timestamp is present at `+8` in known files.

## Video payload detection

The container does not imply a single video codec. The demuxer should inspect the first valid video payload.

Known Annex-B signatures:

- HEVC VPS often starts `00 00 00 01 40 ...` (NAL type 32).
- H.264 commonly exposes SPS/PPS/IDR NAL types 7/8/5.

Detection must examine NAL semantics rather than relying on the filename extension. Several XMEye exports are named `.h264` while containing HEVC.

## Timing

The reference parser estimates frame rate by counting video frames between increasing XMEye timestamps. One known sample resolves to approximately 17 fps. Native VLC timing is still under implementation; timestamps should be preferred over a hardcoded FPS when reliable.

## Extensibility rule

Do not encode codec-specific assumptions into the generic XMEye record parser. New variants should add payload detectors/handlers while reusing the FC/FD/F9/FA transport parser when applicable.
