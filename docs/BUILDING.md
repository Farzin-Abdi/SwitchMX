# Building SwitchMX

Everything needed to build the app from source, run the desktop test build and find your way around the code.
For what the app does, see the [README](../README.md).

## Building for Switch

Install the devkitPro toolchain and the libraries:

```sh
sudo dkp-pacman -S switch-dev switch-libmpv switch-ffmpeg switch-sdl2 switch-mesa switch-glad \
    switch-libass switch-freetype switch-harfbuzz switch-libjpeg-turbo switch-ntfs-3g switch-lwext4
```

Two libraries are built from source rather than kept in this repository, because shipping the
compiled copies without their sources would not meet their licences:

```sh
# MuPDF 1.28.6 — reading PDF, EPUB, CBZ and DOCX
curl -LO https://mupdf.com/downloads/archive/mupdf-1.28.6-source.tar.gz
tar xf mupdf-1.28.6-source.tar.gz
tools/build_mupdf.sh mupdf-1.28.6-source

# libsmb2 6.3 — SMB shared folders
curl -L https://github.com/sahlberg/libsmb2/archive/refs/tags/libsmb2-6.3.tar.gz | tar xz
tools/build_libsmb2.sh libsmb2-libsmb2-6.3
```

libusbhsfs is built by `make` itself, from the copy in `third_party/libusbhsfs`. Then:

```sh
make
```

`make` also generates `source/font_data.c` and `source/codepages.c` from the typefaces in
`third_party/` — they are produced rather than stored, so a clone stays small.

This produces `SwitchMX.nro`. Copy it to `sdmc:/switch/SwitchMX/SwitchMX.nro`.

> **Launch it in title-override mode** (hold **R** while starting any game) for full memory.
> Applet mode (from the album) works too, but large or high-bitrate files can run out of memory.
> In that case lower *Settings → Advanced → Demuxer memory*.

Data, history and screenshots are stored in `sdmc:/switch/SwitchMX/`.

### HOME Menu icon

On first start the app offers to add an icon to the HOME Menu; later it's an on/off switch in *Settings → Advanced*.
It needs custom firmware with sigpatches and your console's `prod.keys` (from Lockpick_RCM) somewhere usual on the SD
card, such as `sdmc:/switch/prod.keys`. If the keys aren't there yet, the choice is remembered and the icon is added by
itself the next time SwitchMX starts with them present. The icon's files are built on your console with those keys;
the keys are not copied or sent anywhere, and no keys or prebuilt packages ship with SwitchMX. The icon starts
`sdmc:/switch/SwitchMX/SwitchMX.nro`, so keep the app there.

## Desktop test build (macOS / Linux)

The UI also runs on a desktop for fast iteration. The mouse acts as a finger, ctrl+scroll or
shift+drag acts as a pinch, and the keyboard acts as the controller (Enter = A, Esc = B,
X/Y, Q/E = L/R, 1/3 = ZL/ZR, Tab = +, M = −, arrows / WASD = D-pad).

```sh
brew install sdl2 mpv          # or your distro's SDL2 + libmpv dev packages
make -f Makefile.desktop
SWITCHMX_ROOT=~/Movies/ ./build-desktop/switchmx
```

## Code layout

| File | Purpose |
|---|---|
| `source/platform_switch.cpp` | libnx: input, touch, shared fonts, battery, backlight, keyboard |
| `source/platform_desktop.cpp` | SDL backend for desktop development |
| `source/input.cpp` | Gesture recognizer: tap, double-tap, long-press, pan with velocity, pinch |
| `source/ui.cpp`, `icons.cpp` | Immediate-mode vector UI on NanoVG: themes, animation, spatial D-pad focus, kinetic scrolling |
| `source/player.cpp` | libmpv wrapper (OpenGL render API, observed properties, commands) |
| `source/home.cpp` | Library, Recent, Favorites, Streams and Settings screens |
| `source/player_screen.cpp` | Player OSD, gestures, HUD and side panels |
| `source/archiver.cpp`, `fileops.cpp` | Archive engine and background file jobs |
| `source/manager.cpp` | File manager actions, job progress and text viewer |
| `source/playlists.cpp` | Playlists and Liked screens |
| `source/coverart.cpp` | Finds, decodes and caches cover art in the background |
| `source/forwarder.cpp` | Builds and installs the HOME Menu icon |
| `source/config.cpp` | Table-driven settings: adding one line here adds an option to the UI |

Settings are table-driven: to add an option, add a field to `Settings` in `config.hpp` and one line to
`buildDefs()` in `config.cpp`. It then appears in the settings screen and is saved automatically.
