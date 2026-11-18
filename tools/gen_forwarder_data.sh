#!/bin/bash
# Regenerates source/forwarder_data.c. Needs a hacBrewPack binary and the forwarder's built exefs:
#   tools/gen_forwarder_data.sh path/to/hacbrewpack path/to/exefs work-dir
# The keys used here are generated from a label: they are fake, which is the point. The templates
# come out in plain form, so nothing in the result depends on them.
set -e
HBP=$1; EXEFS=$2; W=$3; R=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$W/control"
python3 -I - "$W/fake.keys" "$W/control/control.nacp" "$R" <<'PY'
import hashlib, struct, sys
keys, nacp, root = sys.argv[1:4]
def k(label, n): return hashlib.sha256(label.encode()).hexdigest()[:n * 2]
with open(keys, 'w') as f:
    f.write('header_key = %s\n' % k('switchmx-fake-header', 32))
    for i in range(4):
        f.write('key_area_key_application_%02x = %s\n' % (i, k('fake-kaek%d' % i, 16)))
        f.write('key_area_key_ocean_%02x = %s\nkey_area_key_system_%02x = %s\n' % (i, k('o%d' % i, 16), i, k('s%d' % i, 16)))
d = bytearray(open(root + '/third_party/forwarder/control.nacp', 'rb').read())
for lang in range(16):
    o = lang * 0x300 + 0x200
    d[o:o + 0x100] = b'Farzin Abdi'.ljust(0x100, b'\0')
d[0x3025] = 0                                     # no profile picker when it starts
d[0x3080:0x3090] = bytes(16)                      # no save data
open(nacp, 'wb').write(d)
PY
cp "$R/icon.jpg" "$W/control/icon_AmericanEnglish.dat"
"$HBP" -k "$W/fake.keys" --titleid 0100534D58000000 --exefsdir "$EXEFS" --controldir "$W/control" --noromfs --nologo \
    --nosignncasig2 --tempdir "$W/temp" --ncadir "$W/nca" --nspdir "$W/nsp" --backupdir "$W/bak" --keepncadir >/dev/null
# Largest plain .nca is control, smallest is program for this content.
META=$(ls "$W"/nca/*.cnmt.nca)
set -- $(ls -S "$W"/nca/*.nca | grep -v cnmt)
CONTROL=$1; PROGRAM=$2
# The same launcher with permissions in the layout firmware before 19.0.0 expects.
rm -rf "$W/exefs_legacy" "$W/legacy"; mkdir -p "$W/legacy"; cp -R "$EXEFS" "$W/exefs_legacy"
"${DEVKITPRO:-/opt/devkitpro}/tools/bin/npdmtool" "$R/third_party/forwarder/SwitchMX_legacy.json" "$W/exefs_legacy/main.npdm" 2>/dev/null
"$HBP" -k "$W/fake.keys" --titleid 0100534D58000000 --exefsdir "$W/exefs_legacy" --controldir "$W/control" --noromfs --nologo \
    --nosignncasig2 --tempdir "$W/legacy/temp" --ncadir "$W/legacy/nca" --nspdir "$W/legacy/nsp" --backupdir "$W/legacy/bak" --keepncadir >/dev/null
set -- $(ls -S "$W"/legacy/nca/*.nca | grep -v cnmt)
LEGACY=$2
c++ -std=c++17 -I"$R/source" -c "$R/tools/gen_forwarder_data.cpp" -o "$W/gen.o"
cc -I"$R/source" -c "$R/source/minicrypto.c" -o "$W/mc.o"
echo 'const unsigned int g_fwdProgramSize=0,g_fwdControlSize=0,g_fwdMetaSize=0,g_fwdProgramLegacySize=0; const unsigned char g_fwdProgram[1]={0},g_fwdControl[1]={0},g_fwdMeta[1]={0},g_fwdProgramLegacy[1]={0};' > "$W/stub.c"
cc -c "$W/stub.c" -o "$W/stub.o"
c++ -std=c++17 -I"$R/source" -c "$R/source/forwarder.cpp" -o "$W/fwd.o"
# forwarder.cpp words its errors through the translation tables.
c++ -std=c++17 -I"$R/source" -c "$R/source/i18n.cpp" -o "$W/i18n.o"
cat "$R"/source/lang_tables.cpp "$R"/source/lang_??.cpp > "$W/lang.cpp"
c++ -std=c++17 -I"$R/source" -c "$W/lang.cpp" -o "$W/lang.o"
c++ -o "$W/gen" "$W/gen.o" "$W/fwd.o" "$W/mc.o" "$W/stub.o" "$W/i18n.o" "$W/lang.o"
"$W/gen" "$W/fake.keys" "$PROGRAM" "$CONTROL" "$META" "$LEGACY" > "$R/source/forwarder_data.c"
c++ -std=c++17 -I"$R/source" -c "$R/tools/forwarder_test.cpp" -o "$W/test.o"
cc -c "$R/source/forwarder_data.c" -o "$W/data.o"
c++ -o "$W/test" "$W/test.o" "$W/fwd.o" "$W/mc.o" "$W/data.o" "$W/i18n.o" "$W/lang.o"
"$W/test" "$W/fake.keys" "$W/nca"
