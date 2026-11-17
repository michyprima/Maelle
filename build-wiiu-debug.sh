#!/bin/sh
# Build Wii U debug variants of Maelle, each with its own build folder, app file and title, so they can sit next to
# the regular build. The folder is cleared first, since make doesn't notice when only the flags change.
# Usage: ./build-wiiu-debug.sh <variant>...
#   heartbeat    Progress counters and timings in heartbeat.log, 4 times a second
#   timeline     Heartbeat, plus the GPU 3D renderer waiting for the GPU between parts of a frame to time each exactly
#   nocrackfill  Heartbeat without the GPU 3D renderer filling cracks between polygons, to compare frame times
#   capture      Hold L+R+Y (ZL+ZR+Y on the GamePad) to record 24 frames at each step of the pipeline to capture.bin
#   statedebug   Heartbeat, plus snapshots of what decides whether the CPUs run in statelog.txt around state loads
#   gpudump      Polygons, 3D layers and GPU renderer frames dumped every 60 frames, and frames drawn in software too
#   threadprobe  Every thread's state on request over UDP, answered from each core, for freezes
#   netlog       A line for every Wi-Fi network event broadcast to UDP port 4405, for Wi-Fi problems
#   profile      Where the emulator thread spends its time, shown on screen
#   jittest      Runs the JIT against the interpreter before starting a game
#   romtest      Runs tests/rom's ROM with the interpreter and the JIT, timing them, with the results in romtest.txt
#   jitsplit     Heartbeat, plus how long each step of compiling JIT blocks takes
#   drcdebug     What's known about the GamePad's display on the TV, with combos to try getting it back
#   frametime    Heartbeat, plus where the emulator thread spends each frame, listing every slow frame, and samples
#                of where its core was during slow frames in trace.bin
#   nostrips     frametime without the GPU 3D renderer drawing flat polygons a row at a time, to compare frame times
#   frameslots   frametime without the samples, which slow the emulator down, for truer times
#   framelog     The time of every emulator and UI frame in framelog.bin, cheap enough to measure dropped frames
#                and what a change costs; tests/framelog/report.py sums it up and compares two of them
#   audiocap     framelog, plus the audio the emulator made and the audio played in audio-produced.pcm and
#                audio-output.pcm; press ZL + ZR + Y to mark when you hear something
#   romload      How long loading the ROM into RAM took, in romload.txt
#   frameart     How long drawing the screen frames took, in frameart.txt
#   nandlog      Every NAND and SD card command, interrupt and written block in nandlog.txt, for saves that hang
#   gpucrc       A hash of every frame the GPU 3D renderer finishes in gpucrc.bin, with the emulated frame it was
#                handed over in, to check that the GPU's output is the same between builds
set -e
cd "$(dirname "$0")"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC}
BASE='-g -Ofast -fno-allow-store-data-races -flto -ffunction-sections -DESPRESSO -mcpu=750 -meabi -mhard-float $(INCLUDE) -D__WIIU__ -D__WUT__ -DENDIAN_BIG -DNO_FDOPEN -DLOG_LEVEL=0'

[ $# -gt 0 ] || { sed -n '2,30p' "$0"; exit 1; }
for variant in "$@"; do
    case $variant in
        heartbeat) flags='-DHEARTBEAT'; name=maelle-hb; title='Maelle Heartbeat' ;;
        timeline) flags='-DHEARTBEAT -DGPU_TIMELINE'; name=maelle-timeline; title='Maelle GPU Timeline' ;;
        nocrackfill) flags='-DHEARTBEAT -DGPU_NO_CRACK_FILL'; name=maelle-nofill; title='Maelle No Crack Fill' ;;
        capture) flags='-DFRAME_CAPTURE'; name=maelle-capture; title='Maelle Capture' ;;
        statedebug) flags='-DSTATE_DEBUG -DHEARTBEAT'; name=maelle-statedebug; title='Maelle State Debug' ;;
        gpudump) flags='-DGPU_DUMP'; name=maelle-dump; title='Maelle Dump' ;;
        threadprobe) flags='-DTHREAD_PROBE'; name=maelle-probe; title='Maelle Probe' ;;
        netlog) flags='-DNET_LOG'; name=maelle-netlog; title='Maelle Net Log' ;;
        profile) flags='-DPROFILE_EMU'; name=maelle-profile; title='Maelle Profile' ;;
        jittest) flags='-DJIT_SELFTEST'; name=maelle-jittest; title='Maelle JIT Test' ;;
        romtest) flags='-DROM_TEST'; name=maelle-romtest; title='Maelle ROM Test' ;;
        jitsplit) flags='-DHEARTBEAT -DJIT_SPLIT'; name=maelle-jitsplit; title='Maelle JIT Split' ;;
        drcdebug) flags='-DDRC_DEBUG'; name=maelle-drcdebug; title='Maelle GamePad Debug' ;;
        frametime) flags='-DHEARTBEAT -DPROFILE_EMU'; name=maelle-frametime; title='Maelle Frame Times' ;;
        nostrips) flags='-DHEARTBEAT -DPROFILE_EMU -DGPU_NO_STRIPS'; name=maelle-nostrips; title='Maelle No Strips' ;;
        frameslots) flags='-DHEARTBEAT -DPROFILE_EMU -DNO_FRAME_SAMPLES'; name=maelle-frameslots; title='Maelle Frame Slots' ;;
        framelog) flags='-DFRAME_LOG'; name=maelle-framelog; title='Maelle Frame Log' ;;
        audiocap) flags='-DFRAME_LOG -DAUDIO_CAPTURE'; name=maelle-audiocap; title='Maelle Audio Capture' ;;
        romload) flags='-DROM_LOAD_TEST'; name=maelle-romload; title='Maelle ROM Load' ;;
        frameart) flags='-DFRAME_ART_TEST'; name=maelle-frameart; title='Maelle Frame Art' ;;
        nandlog) flags='-DNAND_LOG'; name=maelle-nandlog; title='Maelle NAND Log' ;;
        gpucrc) flags='-DGPU_CRC'; name=maelle-gpucrc; title='Maelle GPU CRC' ;;
        *) echo "Unknown variant: $variant"; exit 1 ;;
    esac
    rm -rf "build-wiiu-$name"
    make -j8 BUILD="build-wiiu-$name" NAME="$name" APP_NAME="$title" \
        CXXFLAGS="$BASE -Wall -Wextra $flags" CFLAGS="$BASE $flags"
    echo "Built $name.wuhb ($title)"
done
