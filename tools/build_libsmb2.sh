#!/bin/sh
# Builds libsmb2 (LGPL-2.1) for the Switch into third_party/libsmb2/{include,lib}.
#   tools/build_libsmb2.sh <unpacked libsmb2-6.3 source> [desktop]
# Source: https://github.com/sahlberg/libsmb2/archive/refs/tags/libsmb2-6.3.tar.gz
# No Kerberos (NTLM sign-in, as Windows, macOS, Samba and NAS shares use by default).
# With "desktop", builds it for this computer into build-desktop/deps instead (for the desktop build).
set -e
SRC=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DKP=${DEVKITPRO:-/opt/devkitpro}
B=$(mktemp -d)
if [ "$2" = desktop ]; then
    OUT=$ROOT/build-desktop/deps
    cmake -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_EXAMPLES=OFF \
        -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
else
    OUT=$ROOT/third_party/libsmb2
    cmake -S "$SRC" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$DKP/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF >/dev/null
fi
cmake --build "$B" -j8 >/dev/null
mkdir -p "$OUT/lib" "$OUT/include/smb2"
cp "$(find "$B" -name 'libsmb2.a' | head -1)" "$OUT/lib/"
cp "$SRC"/include/smb2/*.h "$OUT/include/smb2/"
rm -rf "$B"
echo "libsmb2 -> $OUT"
