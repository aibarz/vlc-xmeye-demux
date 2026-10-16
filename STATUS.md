# STATUS

## 2026-10-16

### Confirmed

- XMEye private wrapper markers FC/FD/F9/FA identified on real recordings.
- A reference Python extractor successfully recovers an HEVC Annex-B stream.
- A known recording is HEVC despite its `.h264` extension.
- Remuxing with video stream copy proves payload can be preserved without re-encoding.
- VLC 3.0.24 is the initial Windows target.

### Repository

- Initial native demux project structure created.
- Reference parser copied from `pc-admin`.
- Native VLC module skeleton in progress.

### Next

1. Finish structural probe and payload codec detector in C.
2. Implement sequential FC/FD/F9/FA reader.
3. Feed HEVC frames directly to VLC ES/packetizer.
4. Compile `libxmeye_plugin.dll` against VLC 3.0.24.
5. Test against XMEye sample without altering the source recording.
6. Add G.711 A-law and additional XMEye variants as samples arrive.

### Read-only probe results

Three current private XMEye samples were scanned structurally without extraction or decoding. All three show the same FC/FD/F9 wrapper family, zero invalid records, and HEVC parameter sets in FC keyframes. This validates a shared first demux path for the current files.

### Build environment

The t520 currently has no native C toolchain (`gcc`, `clang`, `cl`, `cmake`, `meson`, `ninja` absent). Compilation should therefore use a controlled external/CI toolchain rather than burdening the thin client unnecessarily.
