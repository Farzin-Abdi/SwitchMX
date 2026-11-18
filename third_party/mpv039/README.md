# mpv 0.39.0 MKV demuxer (patched)

`demux/demux_mkv.c` replaces the object of the same name in devkitPro's `switch-libmpv` 0.39.0. It is
linked into SwitchMX directly, so the archive's copy is never pulled in. The other files are the headers
it needs, unchanged from the mpv v0.39.0 tag, plus generated `demux/ebml_types.h` and a minimal `config.h`.

The only change is the block limit in `probe_if_image()`; see the comment there. If `switch-libmpv` moves
to another version, re-vendor this file from that tag and re-check that struct offsets still match.

mpv is licensed under LGPLv2.1+ / GPLv2+; see `LICENSE.LGPL` and `Copyright`.
