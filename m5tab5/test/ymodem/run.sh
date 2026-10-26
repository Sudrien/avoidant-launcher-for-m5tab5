#!/bin/bash
# SPDX-License-Identifier: MIT
#
# The receiver against a real sender, through a pair of pipes:
#   YMODEM and YMODEM-G, 1 KB blocks, a batch of an odd size, an empty
#   file and one over 1 MB; refusing to replace a file without -f, and
#   replacing it with -f; and a transfer cut partway, which must fail
#   and leave nothing under the name.
#
#   make && ./run.sh            (SZ=... if sz is not on PATH)
set -eu
cd "$(dirname "$0")"
SZ=${SZ:-sz}
command -v "$SZ" >/dev/null || { echo "no $SZ (lrzsz); set SZ=" >&2; exit 2; }
make -s rx_host

cross() {   # cross "<receiver>" "<sender>": each one's stdout to the other's stdin
    python3 - "$1" "$2" <<'PY'
import os, subprocess, sys
r1, w1 = os.pipe(); r2, w2 = os.pipe()
rx = subprocess.Popen(sys.argv[1], shell=True, stdin=r1, stdout=w2)
tx = subprocess.Popen(sys.argv[2], shell=True, stdin=r2, stdout=w1)
for fd in (r1, w1, r2, w2): os.close(fd)
sys.exit(rx.wait(timeout=180))
PY
}

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir "$T/in"
head -c 1048577 /dev/urandom > "$T/in/big.bin"
head -c 1000 /dev/urandom > "$T/in/small.bin"
: > "$T/in/empty.bin"
fail=0
check() { if "$@"; then echo "ok   $DESC"; else echo "FAIL $DESC"; fail=1; fi; }

for mode in "" "-g"; do
    rm -rf "$T/out"; mkdir "$T/out"
    DESC="batch ${mode:-(C)}"
    check cross "./rx_host $mode $T/out" "cd $T/in && $SZ --ymodem -k big.bin small.bin empty.bin 2>/dev/null"
    for f in big small empty; do DESC="  $f.bin ${mode:-(C)}"; check cmp -s "$T/in/$f.bin" "$T/out/$f.bin"; done
done

DESC="refuses to replace without -f"
check bash -c "! { $(declare -f cross); cross './rx_host $T/out' 'cd $T/in && $SZ --ymodem -k small.bin 2>/dev/null'; }"
head -c 500 /dev/urandom > "$T/in/small.bin"
DESC="replaces with -f"
check cross "./rx_host -f $T/out" "cd $T/in && $SZ --ymodem -k small.bin 2>/dev/null"
DESC="  and the new one is there"; check cmp -s "$T/in/small.bin" "$T/out/small.bin"

head -c 3000000 /dev/urandom > "$T/in/cut.bin"
for mode in "-g" ""; do
    DESC="cut partway fails ${mode:-(C)}"
    check bash -c "! { $(declare -f cross); cross './rx_host $mode $T/out' 'cd $T/in && $SZ --ymodem -k cut.bin 2>/dev/null | head -c 500000; sleep 12'; }"
    DESC="  and leaves no cut.bin or cut.bin.part"
    check test ! -e "$T/out/cut.bin" -a ! -e "$T/out/cut.bin.part"
done
exit $fail
