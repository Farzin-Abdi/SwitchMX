#!/bin/sh
# Builds a minimal libmpv (and the few libraries it needs) into build-desktop/deps
# for the desktop test build. Only FFmpeg is taken from the system (brew install ffmpeg).
#
#   sh tools/build-desktop-deps.sh
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="$ROOT/build-desktop/deps"
SRC="$ROOT/build-desktop/deps-src"
mkdir -p "$PREFIX" "$SRC"

# devkitPro puts its own pkg-config first in PATH; use the host one.
for pc in /opt/homebrew/bin/pkg-config /usr/local/bin/pkg-config /usr/bin/pkg-config; do
    [ -x "$pc" ] && { export PKG_CONFIG="$pc"; break; }
done
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig"

"$PKG_CONFIG" --exists libavcodec || { echo "FFmpeg not found: brew install ffmpeg"; exit 1; }

fetch() { # name url
    [ -d "$SRC/$1" ] && return
    echo "==> downloading $1"
    mkdir -p "$SRC/$1"
    curl -sSfL "$2" | tar -xz -C "$SRC/$1" --strip-components=1
}

clone() { # name url tag
    [ -d "$SRC/$1" ] && return
    echo "==> cloning $1"
    git clone --quiet --depth 1 --branch "$3" --recursive --shallow-submodules "$2" "$SRC/$1"
}

build() { # name [meson options...]
    name=$1; shift
    [ -f "$SRC/$name/.installed" ] && return
    echo "==> building $name"
    (cd "$SRC/$name" &&
        meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release --wrap-mode=nodownload "$@" >/dev/null &&
        ninja -C build install >/dev/null)
    touch "$SRC/$name/.installed"
}

fetch freetype https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz
build freetype -Dbrotli=disabled -Dbzip2=disabled -Dharfbuzz=disabled -Dpng=disabled

clone fribidi https://github.com/fribidi/fribidi.git v1.0.16
build fribidi -Ddocs=false -Dtests=false -Dbin=false

clone harfbuzz https://github.com/harfbuzz/harfbuzz.git 10.4.0
build harfbuzz -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dicu=disabled -Dfreetype=enabled \
    -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dintrospection=disabled

clone libass https://github.com/libass/libass.git 0.17.3
build libass -Dfontconfig=disabled -Ddirectwrite=disabled -Dasm=disabled -Drequire-system-font-provider=false

clone libplacebo https://code.videolan.org/videolan/libplacebo.git v7.351.0
build libplacebo -Dvulkan=disabled -Dopengl=enabled -Dshaderc=disabled -Dglslang=disabled -Dlcms=disabled \
    -Dlibdovi=disabled -Ddemos=false -Dtests=false -Dunwind=disabled -Dxxhash=disabled

clone mpv https://github.com/mpv-player/mpv.git v0.41.0
build mpv -Dlibmpv=true -Dcplayer=false -Dlua=disabled -Djavascript=disabled -Dlibarchive=disabled \
    -Dlibbluray=disabled -Ddvdnav=disabled -Dcdda=disabled -Duchardet=disabled -Drubberband=disabled \
    -Dzimg=disabled -Dlcms2=disabled -Dvapoursynth=disabled -Dvulkan=disabled -Dshaderc=disabled \
    -Dspirv-cross=disabled -Dmanpage-build=disabled

# Same libarchive version as switch-libarchive and, like it, without a crypto library, so the
# archive code that gets tested here is what runs on the Switch.
LA="$SRC/libarchive-3.7.2"
if [ ! -f "$PREFIX/lib/libarchive.a" ]; then
    echo "==> building libarchive"
    [ -d "$LA" ] || curl -sSfL https://github.com/libarchive/libarchive/releases/download/v3.7.2/libarchive-3.7.2.tar.gz | tar -xz -C "$SRC"
    (cd "$LA" &&
        CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib ./configure --prefix="$PREFIX" --disable-shared \
            --disable-bsdtar --disable-bsdcat --disable-bsdcpio --disable-bsdunzip --disable-acl --disable-xattr \
            --without-openssl --without-nettle --without-mbedtls --without-cng --without-xml2 --without-expat \
            --without-iconv --without-libb2 >/dev/null &&
        sed -i.bak '/ARCHIVE_CRYPTOR_USE/d; /ARCHIVE_HMAC_USE/d' config.h &&
        make -j8 libarchive.la >/dev/null 2>&1 &&
        cp .libs/libarchive.a "$PREFIX/lib/")
    mkdir -p "$PREFIX/include"; cp "$LA/libarchive/archive.h" "$LA/libarchive/archive_entry.h" "$PREFIX/include/"
fi

echo "==> done: $PREFIX"
