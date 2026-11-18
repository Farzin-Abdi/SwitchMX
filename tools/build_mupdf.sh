#!/bin/sh
# Builds MuPDF (AGPL-3.0) for the Switch into third_party/mupdf/{include,lib}.
#   tools/build_mupdf.sh <unpacked mupdf-1.28.6-source>
# Source: https://mupdf.com/downloads/archive/mupdf-1.28.6-source.tar.gz
# FreeType, HarfBuzz, zlib and libjpeg come from devkitPro's portlibs (the app already links them);
# comics in RAR/7z/tar go through the app's libarchive; the rest (jbig2dec, openjpeg, lcms2mt, gumbo, brotli) are MuPDF's own copies. No JavaScript,
# OCR, barcodes or document export; the big CJK fonts are left out (the Switch's own font stands in), and
# so are the fonts for historic scripts.
set -e
SRC=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DKP=${DEVKITPRO:-/opt/devkitpro}
PL=$DKP/portlibs/switch
BIN=$DKP/devkitA64/bin
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
SYS="-I$PL/include -I$PL/include/freetype2 -I$PL/include/harfbuzz -I$DKP/libnx/include -D__SWITCH__"
cd "$SRC"
make -j8 build=release OS=switch \
    CC="$BIN/aarch64-none-elf-gcc" CXX="$BIN/aarch64-none-elf-g++" AR="$BIN/aarch64-none-elf-ar" \
    XCFLAGS="$ARCH $SYS -include $ROOT/tools/mupdf_nx.h -O2 -DFZ_ENABLE_ICC=1 -DFZ_ENABLE_HYPHEN_ALL=0 -DTOFU_HISTORIC -DTOFU_SIL -DFZ_PLOTTERS_G=1 -DFZ_PLOTTERS_RGB=1 -DFZ_PLOTTERS_CMYK=1 -DFZ_PLOTTERS_N=1" \
    HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_PTHREAD=no HAVE_OBJCOPY=no \
    mujs=no tesseract=no barcode=no extract=no tofu_cjk=yes \
    archive=yes HAVE_SYS_LIBARCHIVE=yes SYS_LIBARCHIVE_CFLAGS="-I$PL/include" \
    USE_SYSTEM_FREETYPE=yes USE_SYSTEM_HARFBUZZ=yes USE_SYSTEM_ZLIB=yes USE_SYSTEM_LIBJPEG=yes \
    SYS_FREETYPE_CFLAGS="-I$PL/include/freetype2" SYS_HARFBUZZ_CFLAGS="-I$PL/include/harfbuzz" \
    SYS_ZLIB_CFLAGS="-I$PL/include" SYS_LIBJPEG_CFLAGS="-I$PL/include" \
    libs
OUT=$ROOT/third_party/mupdf
rm -rf "$OUT/include" "$OUT/lib"
mkdir -p "$OUT/lib"
cp -R include "$OUT/include"
cp build/release-tofu_cjk-archive/libmupdf.a build/release-tofu_cjk-archive/libmupdf-third.a "$OUT/lib/"
cp COPYING "$OUT/COPYING"
echo "MuPDF built into $OUT"
