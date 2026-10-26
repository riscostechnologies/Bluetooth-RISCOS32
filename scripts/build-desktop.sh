#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 RISC OS Technologies
# Build the !Bluetooth desktop package: the icon bar task, the Configure
# plug-in, their sprites, and the modules they load.  Output:
#   build/desktop/app          the tree as it is installed
#   build/desktop/Bluetooth.zip  the same tree, with RISC OS filetypes
#
# Environment:
#   RISCOS_CC   GCCSDK cross gcc (default ~/gccsdk/cross/bin/arm-unknown-riscos-gcc)
#   MKZIP       RISC OS zip builder (default hd4-new/mkzip.py)
set -eu
cd "$(dirname "$0")/.."
cc=${RISCOS_CC:-$HOME/gccsdk/cross/bin/arm-unknown-riscos-gcc}
bin=$(dirname "$cc")
mkzip=${MKZIP:-$HOME/Downloads/hd4-new/mkzip.py}
app=build/desktop/app
mkdir -p build/desktop/inc build/desktop/o "$app/!Bluetooth/Modules" "$app/!BTSetup"
for m in SharedULib USBDriver DWCDriver
do
    [ -f "desktop/modules/$m,ffa" ] || {
        echo "desktop/modules/$m,ffa is missing: see README, 'Third-party modules'" >&2
        exit 1
    }
done

for header in btsettings btstack btwimp
do
    cp "client/h/$header" "build/desktop/inc/$header.h"
done

for source in btsettings btwimp btstack btbar btsetup
do
    "$cc" -std=gnu99 -Wall -Wextra -Werror -O2 \
        -Ibuild/desktop/inc -Ibuild/include -x c -c "client/c/$source" \
        -o "build/desktop/o/$source.o"
done
"$cc" -static build/desktop/o/btbar.o build/desktop/o/btstack.o \
    build/desktop/o/btsettings.o build/desktop/o/btwimp.o \
    -o build/desktop/BTBar.elf
"$cc" -static build/desktop/o/btsetup.o build/desktop/o/btsettings.o \
    build/desktop/o/btwimp.o -o build/desktop/BTSetup.elf
"$bin/elf2aif" build/desktop/BTBar.elf "$app/!Bluetooth/!RunImage,ff8"
"$bin/elf2aif" build/desktop/BTSetup.elf "$app/!BTSetup/!RunImage,ff8"

# The module the task loads, and the USB stack it needs.
cp build/module/BluetoothVoice.elf "$app/!Bluetooth/Modules/BTVoice,ffa"
# GCCSDK binaries need SharedUnixLibrary; not every machine has it (the
# Titanium did not), so both applications carry a copy.
mkdir -p "$app/!BTSetup/Modules"
cp "desktop/modules/SharedULib,ffa" "$app/!Bluetooth/Modules/SharedULib,ffa"
cp "desktop/modules/DWCDriver,ffa" "$app/!Bluetooth/Modules/DWCDriver,ffa"
# USBDriver 1.35: the ROM's 1.33 is older than the task accepts.
cp "desktop/modules/USBDriver,ffa" "$app/!Bluetooth/Modules/USBDriver,ffa"
cp "desktop/modules/SharedULib,ffa" "$app/!BTSetup/Modules/SharedULib,ffa"

python3 desktop/mksprites.py "$app"
rm -f "$app/!Bluetooth/LoadUSB,feb"
cp 'desktop/!Help,fff' "$app/!Bluetooth/!Help,fff"
cp 'desktop/appfiles/Bluetooth/!Run,feb' 'desktop/appfiles/Bluetooth/!Boot,feb' "$app/!Bluetooth/"
cp 'desktop/appfiles/BTSetup/!Run,feb' 'desktop/appfiles/BTSetup/!Boot,feb' "$app/!BTSetup/"
version=$(sed -n 's/.*VERSION_TEXT "\([^"]*\)".*/\1/p' client/c/btbar | head -1)
sed "s/VERSION/$version/" 'desktop/appfiles/BTSetup/Messages,fff' > "$app/!BTSetup/Messages,fff"

rm -f build/desktop/Bluetooth.zip
python3 "$mkzip" "$app" build/desktop/Bluetooth.zip Bluetooth >/dev/null
ls -l build/desktop/Bluetooth.zip
