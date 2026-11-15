#!/bin/bash
# Builds game.cpp and the core as they are with devkitPPC, using the console's flags, into a program guest/host.cpp runs
# in Unicorn, and builds the host; game.sh calls this for its gcc- modes
# Usage: build.sh <build folder> <Unicorn folder> [mutation number, planted in a copy of the core by patch.py]
set -e
here=$(cd "$(dirname "$0")"; pwd)
core=$(cd "$here/../../../src/core"; pwd)
out=$1; uni=$2; mutation=${3:-0}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
bin=${DEVKITPPC:-$DEVKITPRO/devkitPPC}/bin
[ -x "$bin/powerpc-eabi-g++" ] || { echo "the gcc- modes need devkitPPC in $bin"; exit 1; }
mkdir -p "$out/obj"
if [ "$mutation" != 0 ]; then
    # Only copies that changed get newer, so only they're built again
    rm -rf "$out/new"; mkdir -p "$out/new"; cp -R "$core/" "$out/new/"
    python3 -I "$here/../patch.py" "$out/new" "$mutation" plant
    rsync -rc --delete "$out/new/" "$out/src/"
    core="$out/src"
    mkdir -p "$out/ui/console"; cp "$here/../../../src/ui/console/frame_log.h" "$out/ui/console/"
fi

# As in the Makefile, without wut, which only the network code uses, and with the harness's coreinit for the codegen area;
# FRAME_LOG and AUDIO_CAPTURE give game.cpp the sound, as in the console's debug builds
machine="-DESPRESSO -mcpu=750 -meabi -mhard-float"
dirs=$(cd "$core"; find . -type d -not -path './faad2*' -not -path './net/libslirp*' | sed "s|^\.|-I$core|")
flags="-g -Ofast -fno-allow-store-data-races -flto -ffunction-sections $machine -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0
    -DJIT_GUEST -DFRAME_LOG -DAUDIO_CAPTURE $JIT_TEST_CFLAGS -I$here -I$here/include $dirs -w"
faad="-g -O2 -w -ffunction-sections $machine -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1
    -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DPACKAGE_VERSION=\"2.11.4\" -DAPPLY_DRC -DLC_ONLY_DECODER
    -DDISABLE_SBR -I$core/faad2"

# Everything again when a header or the flags change, otherwise only what's newer than its object
stamp="$out/obj/flags"
if [ "$(cat "$stamp" 2>/dev/null)" != "$flags" ] ||
    [ -n "$(find "$core" "$here" "$here/../shim/coreinit" -name '*.h' -newer "$stamp" 2>/dev/null | head -1)" ]; then
    rm -f "$out"/obj/*.o
fi
pids=()
build() {
    local src=$1 obj=$2; shift 2
    if [ ! -e "$obj" ] || [ "$src" -nt "$obj" ]; then "$@" -c "$src" -o "$obj" & pids+=($!); fi
    while [ "$(jobs -r | wc -l)" -ge 12 ]; do sleep 0.05; done
}
for f in $(cd "$core"; find . -name '*.cpp' -not -path './net/libslirp*'); do
    # The network code is built as for the console, with its socket headers from wut, after everything else so the
    # harness's coreinit comes first
    extra=(); [ "$f" != ./net/net_slirp.cpp ] || extra=(-D__WIIU__ -D__WUT__ -idirafter "$DEVKITPRO/wut/include")
    build "$core/$f" "$out/obj/$(echo "${f#./}" | tr '/' '_').o" "$bin/powerpc-eabi-g++" $flags "${extra[@]}"
done
for f in $(cd "$core/faad2"; ls *.c); do
    build "$core/faad2/$f" "$out/obj/faad2_${f%.c}.o" "$bin/powerpc-eabi-gcc" $faad
done
build "$here/../game.cpp" "$out/obj/game.o" "$bin/powerpc-eabi-g++" $flags
build "$here/runtime.cpp" "$out/obj/runtime.o" "$bin/powerpc-eabi-g++" $flags
build "$here/network.cpp" "$out/obj/network.o" "$bin/powerpc-eabi-g++" $flags -D__WIIU__ -D__WUT__ -idirafter "$DEVKITPRO/wut/include"
build "$here/../shim/slirp.c" "$out/obj/slirp.o" "$bin/powerpc-eabi-gcc" -O2 -w $machine
for p in "${pids[@]}"; do wait "$p" || { echo "guest build failed"; exit 1; }; done
echo "$flags" > "$stamp"
"$bin/powerpc-eabi-g++" -g -Ofast -flto=auto -Wno-odr $machine "$out"/obj/*.o -Wl,-Ttext-segment=0x02200000 -o "$out/guest.elf"

if [ ! -e "$out/host" ] || [ "$here/host.cpp" -nt "$out/host" ] || [ "$here/layout.h" -nt "$out/host" ]; then
    clang++ -std=c++17 -O2 -w -I"$here" -I"$uni/include" "$here/host.cpp" -L"$uni/build" -lunicorn -Wl,-rpath,"$uni/build" -o "$out/host"
fi
