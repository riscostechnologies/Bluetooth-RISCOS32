#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 RISC OS Technologies
# bv70: the 64-bit protocol core over the 32-bit USB transport.
set -u
cd "$(dirname "$0")/.."
compiler=${RISCOS_MODULE_CC:-$HOME/gccsdk/cross/bin/arm-unknown-riscos-gcc}
mkdir -p build/module build/include
cp port/cmhg/modhead.o build/module/modhead.o
cp port/cmhg/modhead.h build/include/modhead.h
mkdir -p build/include/riscos
for h in port/h/*; do [ -f "$h" ] && cp "$h" "build/include/$(basename "$h").h"; done
for h in port/h/riscos/*; do cp "$h" "build/include/riscos/$(basename "$h").h"; done
skip=" hfp_pcm rfcomm_connect ${SKIP:-} "
objs=''
fails=0
for c in port/c/*; do
  s=$(basename "$c")
  case "$skip" in *" $s "*) continue;; esac
  extra=''
  [ "$s" = module_runtime ] && extra='-fno-builtin'
  if "$compiler" -mmodule -std=gnu99 -Wall -O2 -fno-strict-aliasing $extra -Ibuild/include -Iport/h \
       -x c -c "$c" -o "build/module/$s.o" 2> "build/module/$s.err"; then
    objs="$objs build/module/$s.o"
  else
    fails=$((fails+1)); echo "FAIL $s: $(grep -c 'error' build/module/$s.err) errors"
  fi
done
echo "compile failures: $fails"
[ $fails -eq 0 ] || exit 1
"$compiler" -mmodule -nostdlib build/module/modhead.o $objs -o build/module/BluetoothVoice.elf 2> build/module/link.err || { grep -o "undefined reference to \`[^']*'" build/module/link.err | sort | uniq -c | sort -rn | head -60; exit 1; }
echo linked
