#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 RISC OS Technologies
# Shipping package: build/package/Bluetooth-<version>.zip, holding
#   Bluetooth.!Install          the Wimp installer
#   Bluetooth.Apps.!Bluetooth   the icon bar task and its modules
#   Bluetooth.Apps.!BTSetup     the Configure plug-in
#   Bluetooth.ReadMe
# Run scripts/build-desktop.sh first.
set -eu
cd "$(dirname "$0")/.."
mkzip=${MKZIP:-$HOME/Downloads/hd4-new/mkzip.py}
version=$(sed -n 's/.*VERSION_TEXT "\([^"]*\)".*/\1/p' client/c/btbar | head -1)
short=${version%% *}
out=build/package
rm -rf "$out"
mkdir -p "$out/Bluetooth/Apps"
cp -r 'desktop/installer/!Install' "$out/Bluetooth/"
cp -r 'build/desktop/app/!Bluetooth' 'build/desktop/app/!BTSetup' "$out/Bluetooth/Apps/"
sed "s/VERSION/$version/" desktop/installer/ReadMe,fff > "$out/Bluetooth/ReadMe,fff"
# The installer's icon: the application's sprite under the name !install.
python3 - "$out/Bluetooth/Apps/!Bluetooth/!Sprites,ff9" "$out/Bluetooth/!Install/!Sprites,ff9" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
# Sprite area file: count, first, free; each sprite: next, name[12], ...
count, first = struct.unpack_from('<II', d, 0)
at, keep = first - 4, None
for _ in range(count):
    nxt = struct.unpack_from('<I', d, at)[0]
    name = bytes(d[at + 4:at + 16]).rstrip(b'\0')
    if name == b'!bluetooth':
        keep = bytearray(d[at:at + nxt])
    at += nxt
assert keep is not None, 'no !bluetooth sprite'
keep[4:16] = b'!install'.ljust(12, b'\0')
head = struct.pack('<III', 1, 16, 16 + len(keep))
open(sys.argv[2], 'wb').write(head + keep)
PY
grep -q '^IconSprites' "$out/Bluetooth/!Install/!Boot,feb"
sed -i 's/^WimpSlot.*/&\nIconSprites <Install$Dir>.!Sprites/' "$out/Bluetooth/!Install/!Run,feb"
python3 "$mkzip" "$out/Bluetooth" "$out/Bluetooth-$short.zip" Bluetooth >/dev/null
ls -l "$out/Bluetooth-$short.zip"
