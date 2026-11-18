# FFmpeg header

`url.h` is copied unchanged from FFmpeg 7.1 (`libavformat/url.h`), which is not installed by
devkitPro's FFmpeg package. `source/ffmpeg_protocols.c` needs it to supply libavformat's protocol
list, because devkitPro's build leaves out the `https` protocol (see the comments in that file).

FFmpeg is licensed under the GNU Lesser General Public License, version 2.1 or later; the copy
SwitchMX links against is devkitPro's `switch-ffmpeg` package, built from
<https://ffmpeg.org/releases/> and carrying its own licence text.

Nothing here is compiled on its own: the header only declares types that the FFmpeg library
already provides.
