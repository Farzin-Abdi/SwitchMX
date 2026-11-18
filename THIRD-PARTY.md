# Third-party software in SwitchMX

SwitchMX is released under the **GNU Affero General Public License, version 3 or later**
(see [LICENSE](LICENSE)). That is not an arbitrary choice: SwitchMX links MuPDF, which is AGPL-3.0,
and the AGPL is the strongest licence in the list below, so the finished app has to carry it.

Each component keeps its own licence. The full text of every one is in its folder under
`third_party/`; nothing here replaces those files.

## Libraries linked into the app

| Component | Used for | Licence | Where |
|---|---|---|---|
| [MuPDF](https://mupdf.com) 1.28.6 | PDF, EPUB, CBZ and DOCX reading | AGPL-3.0-or-later | `third_party/mupdf/COPYING` |
| [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs) | USB drives (FAT, NTFS, ext4) | GPL-2.0-or-later, ISC | `third_party/libusbhsfs/` |
| [mpv](https://mpv.io) / libmpv 0.39 | Video and audio playback | LGPL-2.1-or-later | `third_party/mpv039/LICENSE.LGPL` |
| [FFmpeg](https://ffmpeg.org) 7.1 | Decoding, network protocols | LGPL-2.1-or-later | `third_party/ffmpeg71/README.md` |
| [libsmb2](https://github.com/sahlberg/libsmb2) 6.3 | SMB shared folders | LGPL-2.1-or-later | `third_party/libsmb2/` |
| [libarchive](https://libarchive.org) 3.7.2 | Archives (zip, 7z, rar, tar…) | BSD-2-Clause | `third_party/libarchive372/COPYING` |
| [NanoVG](https://github.com/memononen/nanovg) | Drawing the interface | zlib | `third_party/nanovg/LICENSE.txt` |
| [nx-hbloader](https://github.com/switchbrew/nx-hbloader) | The HOME Menu shortcut | ISC | `third_party/forwarder/LICENSE.md` |
| [libnx](https://github.com/switchbrew/libnx), SDL2 | Console support, windowing | ISC, zlib | devkitPro packages |

## Typefaces

All are free to redistribute. The interface offers five per language; see
`kUiFonts` in `source/app.cpp`.

| Typeface | Script | Licence |
|---|---|---|
| Vazirmatn, Sahel, Samim, Shabnam, Parastoo (Saber Rastikerdar) | Persian, Arabic, Latin | SIL OFL 1.1 |
| Cairo, Almarai, Tajawal, Noto Sans Arabic | Arabic | SIL OFL 1.1 |
| IBM Plex Sans Arabic | Arabic, Latin | SIL OFL 1.1 |
| Inter, Open Sans, Lato, Montserrat, PT Sans | Latin, Cyrillic | SIL OFL 1.1 |
| Roboto | Latin, Cyrillic | Apache-2.0 |
| Orbitron (the SwitchMX wordmark) | Latin | SIL OFL 1.1 |

Japanese, Chinese and Korean characters are drawn with the console's own system fonts, which are
not redistributed here.

Most Latin and Arabic faces are stored trimmed to the alphabets SwitchMX needs, which is permitted
by the OFL. The originals are linked above; `tools/` holds the commands used.

## What is **not** included

- **`prod.keys`.** The HOME Menu shortcut is built on the console from keys that are already on
  your SD card. No console keys are in this repository, and none may be added to it.
- **Nintendo code or assets.** None is used.
- **Any media.** Films, music and books are yours.

## Rebuilding the libraries

The prebuilt static libraries under `third_party/*/lib/` are not part of this repository; the
scripts that produce them are:

- `tools/build_mupdf.sh` — MuPDF, from the source tarball named in the script
- `tools/build_libsmb2.sh` — libsmb2 6.3
- `tools/build-desktop-deps.sh` — the desktop build's copies of mpv, FFmpeg and SDL2
- libusbhsfs comes from its own release; drop its `include/` and `lib/` into `third_party/libusbhsfs/`

See [README.md](README.md) for the full build steps.
