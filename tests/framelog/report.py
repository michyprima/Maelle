#!/usr/bin/env python3
# Sums up the framelog.bin that a `./build-wiiu-debug.sh framelog` build of Maelle writes to its folder on the SD card:
# how long the emulator worked on each frame, from the end of one frame's wait for the display to the next being done,
# which shows what a change costs on the console even with the frame limiter on. With two logs of the same save state
# left alone, it also compares the frames where both drew the same number of polygons, since a change to timing lets a
# game drift, and work only compares between frames that show the same thing
# Usage: report.py <framelog.bin> [another to compare] [--window frames]
import statistics, struct, sys

args = sys.argv[1:]
window = 600
if '--window' in args:
    at = args.index('--window')
    window = int(args[at + 1])
    del args[at:at + 2]
if not 1 <= len(args) <= 2:
    sys.exit('Usage: report.py <framelog.bin> [another to compare] [--window frames]')

def load(path):
    # Entries are four big-endian words: a type, then for the emulator's frames (type 2) when the frame was done, when
    # its wait for the display ended, and its polygons; type 1 marks the emulator starting, as it does after a state loads
    data = open(path, 'rb').read()
    entries = [struct.unpack('>4I', data[i:i + 16]) for i in range(0, len(data) - 15, 16)]
    if not entries or entries[0][0] != 0x4E464C47:
        sys.exit(f'{path} is not a frame log')
    speed = entries[0][1]
    runs, run = [], []
    for kind, time, value, polygons in entries[1:]:
        if kind == 1:
            runs.append(run)
            run = []
        elif kind == 2:
            run.append((time, value, polygons))
    runs.append(run)

    # The longest run is the one to measure; each frame gets its work, the time since the last frame, and its polygons
    run = max(runs, key=len)
    ms = lambda ticks: (ticks & 0xFFFFFFFF) * 1000.0 / speed
    return [(ms(f1 - w0), ms(f1 - f0), p1) for (f0, w0, _), (f1, _, p1) in zip(run, run[1:])]

def describe(frames):
    work = sorted(f[0] for f in frames)
    period = [f[1] for f in frames]
    at = lambda part: work[min(len(work) - 1, int(len(work) * part))]
    return (f'work {statistics.mean(work):.3f} ms, median {at(.5):.3f}, 99% under {at(.99):.3f}; '
        f'{len(frames) * 1000 / sum(period):.2f} fps, {sum(w > 16.7 for w in work)} frames over 16.7 ms of work, '
        f'{sum(p > 17.5 for p in period)} late')

logs = [load(path) for path in args]
for path, frames in zip(args, logs):
    print(f'{path}: {len(frames)} frames, {describe(frames)}')
if len(logs) < 2:
    sys.exit()

# Both logs start at the same frame when they're of the same state, so compare them frame by frame from there
a, b = logs
count = min(len(a), len(b))
print(f'\nThe first {count} frames of each, {window} at a time:')
for start in range(0, count - window + 1, window):
    x, y = a[start:start + window], b[start:start + window]
    same = sum(f[2] == g[2] for f, g in zip(x, y))
    first, second = statistics.mean(f[0] for f in x), statistics.mean(g[0] for g in y)
    print(f'  {start:6}: {first:.3f} ms and {second:.3f} ms, {second - first:+.3f}; {same} of {window} frames with the same '
        f'polygons, {statistics.mean(f[2] for f in x):.0f} and {statistics.mean(g[2] for g in y):.0f} on average')

# Only windows where every frame matches count as the same scene, so a stretch that drifted apart is left out
matched = [i for start in range(0, count - window + 1, window)
    if all(a[i][2] == b[i][2] for i in range(start, start + window)) for i in range(start, start + window)]
if matched:
    first, second = statistics.mean(a[i][0] for i in matched), statistics.mean(b[i][0] for i in matched)
    print(f'\n{len(matched)} frames in windows that match throughout: {first:.3f} ms and {second:.3f} ms, '
        f'{second - first:+.3f} ms ({(second / first - 1) * 100:+.2f}%)')
else:
    print('\nNo window matches throughout, so the two logs are not of the same scenes')
