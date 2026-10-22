# NVENC 4:2:0 decoder fixtures

`app/streaming/video/nvenc420-test-frame.h` embeds one IDR per Linux-host NVENC
4:2:0 mode (`h264-8-420-nvenc`, `hevc-10-420-nvenc`). Both are 1280x720 SMPTE
HD bars (no desktop content), 4:2:0, limited range, BT.709 matrix and
primaries, sRGB transfer (13): the exact VUI the host writes and checks in its
own startup probe. They were made with FFmpeg 8.1 (libx264 / libx265), SEI
NAL units removed, then verified with `trace_headers`:

```sh
ffmpeg -f lavfi -i smptehdbars=size=1280x720:rate=60 -frames:v 1 \
  -vf scale=out_color_matrix=bt709:out_range=tv,format=yuv420p \
  -c:v libx264 -profile:v high -preset veryslow -crf 36 \
  -x264-params ref=1:colorprim=bt709:transfer=iec61966-2-1:colormatrix=bt709:range=tv:chromaloc=0 \
  -bsf:v filter_units=remove_types=6 -f h264 h264.h264
ffmpeg -f lavfi -i smptehdbars=size=1280x720:rate=60 -frames:v 1 \
  -vf scale=out_color_matrix=bt709:out_range=tv,format=yuv420p10le \
  -c:v libx265 -profile:v main10 -preset veryslow -crf 34 \
  -x265-params info=0:colorprim=bt709:transfer=iec61966-2-1:colormatrix=bt709:range=limited:chromaloc=0 \
  -f hevc hevc.h265
```

H.264: `profile_idc` 100, `chroma_format_idc` 1, 8-bit, IDR (1158 bytes,
SHA-256 `96fd74a4…9d88`). HEVC: `general_profile_idc` 2 (Main 10),
`chroma_format_idc` 1, 10-bit, IDR_N_LP (931 bytes, SHA-256 `d7b8b461…b69c`).
Trailing zero padding is outside the packet length. The host's
`tests/unit/test_cbs_nvenc_420.cpp` checks the same two payloads with its SPS
gate.

This suite decodes both payloads with the pinned client FFmpeg in software and
exercises `plankNvenc420FrameMatches` for every field it guards. On macOS it
also decodes each payload through VideoToolbox with hardware required and
checks the returned surface (`nv12` / `p010le`) and colour metadata: that is
the exact-profile hardware test the client runs before it accepts a
VideoToolbox path for these profiles. It does not prove presentation, colour
appearance or live-stream performance; those are checked on a real stream.
