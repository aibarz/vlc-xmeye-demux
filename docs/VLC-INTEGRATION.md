# VLC integration

## Intended module type

A native VLC `demux` module. This is not a Lua extension and not an external launcher.

Expected installed form on Windows:

```
plugins\demux\libxmeye_plugin.dll
```

VLC's module probing should invoke XMEye detection automatically. The module accepts the stream only when structural validation succeeds; otherwise it returns `VLC_EGENERIC` and VLC continues probing its normal demuxers.

## Pipeline

1. Peek enough bytes to validate multiple XMEye records.
2. Locate the first valid video payload.
3. Detect payload codec (initially HEVC/H.264).
4. Register the corresponding VLC video elementary stream.
5. Parse records sequentially.
6. Strip only XMEye private headers.
7. Send the original encoded payload bytes to VLC.
8. Add G.711 A-law audio as a second ES when present.

## Non-goals

- No transcoding.
- No MKV/MP4 temporary file.
- No persistent background service.
- No dependency on the misleading `.h264` filename extension.

## Compatibility target

First implementation targets VLC 3.0.24. The code should keep format parsing isolated enough to port to VLC 4 later.
