#!/usr/bin/env python3
# Sums up the trace.bin that a `./build-wiiu-debug.sh frameslots` build of NooDS U writes to its folder on the SD card
# next to heartbeat.log: every emulated frame's record of where the emulator thread spent its time, by profiling slot and
# by scheduled task, how the 2D engines drew the frame and how long the GPU took over its 3D. Frames are grouped into all
# of them, slow ones where the emulator thread worked, and slow ones where it waited for a 2D engine at the end of the
# frame, since saving time on the emulator thread only helps the first kind; `frametime` builds write the same records
# for the slow frames only, along with samples this doesn't read yet
# Frames end at line 192, so a record's wall time is the display wait and the previous frame's V-blank lines, then this
# frame's lines up to 192 (reached line 192 counts from line 0) and the wait for the 2D engines to finish
# Usage: report.py <trace.bin> [--from frame] [--to frame]
import collections, os, re, struct, sys

args = sys.argv[1:]
first, last = 0, 1 << 32
for flag in ('--from', '--to'):
    if flag in args:
        at = args.index(flag)
        if flag == '--from': first = int(args[at + 1])
        else: last = int(args[at + 1])
        del args[at:at + 2]
if len(args) != 1:
    sys.exit('Usage: report.py <trace.bin> [--from frame] [--to frame]')

# The names of the profiling slots, counts and tasks, in the order the core numbers them
core = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../src/core')
def enum(path, name):
    text = open(os.path.join(core, path)).read()
    body = re.search(r'enum %s \{(.*?)\};' % name, text, re.S).group(1)
    return [m.group(1) for m in re.finditer(r'^\s*([A-Z0-9_]+)\s*(?:,|$)', body, re.M) if not m.group(1).endswith('COUNT')]
slots = enum('emu_profile.h', 'EmuProfileSlot')
counters = enum('emu_profile.h', 'EmuCountSlot')
tasks = enum('core.h', 'SchedTask')[:-1]

# A header of ten big-endian words, then chunks of a tag, a size in words and the words; FRAM chunks hold a record's
# index and the record, the rest are samples and JIT blocks for later readers
data = open(args[0], 'rb').read()
header = struct.unpack('>10I', data[:40])
if header[0] != 0x4E545243:
    sys.exit(f'{args[0]} is not a frame trace')
epCount, ecCount, recordWords = header[6], header[7], header[8]
if (epCount, ecCount) != (len(slots), len(counters)):
    sys.exit(f'the trace has {epCount} slots and {ecCount} counts, the source has {len(slots)} and {len(counters)}')
if recordWords != 2 + epCount + ecCount + 6 + 64 + 16 + 2:
    sys.exit(f'the trace\'s records are {recordWords} words, which this script doesn\'t know')
records, at = {}, 40
while at + 8 <= len(data):
    tag, words = data[at:at + 4], struct.unpack('>I', data[at + 4:at + 8])[0]
    if tag == b'FRAM':
        values = struct.unpack('>%dI' % words, data[at + 8:at + 8 + words * 4])
        records[values[0]] = values[1:]
    at += 8 + words * 4

def decode(v):
    # Split a record into named fields, all times in microseconds; 2D times are since the frame started
    r, i = {}, 2
    r['frame'], r['wall'] = v[0], v[1]
    r['slots'] = dict(zip(slots, v[i:i + epCount])); i += epCount
    r['counts'] = dict(zip(counters, v[i:i + ecCount])); i += ecCount
    r['gpu frames'], r['gpu build'], r['gpu draw'], r['2D waits 3D'], r['ring level'], r['ring low'] = v[i:i + 6]; i += 6
    r['tasks'] = dict(zip(tasks, v[i:i + 64])); i += 64
    for engine in 'AB':
        r['2D ' + engine] = dict(zip(('first start', 'first end', 'last end', 'busy', 'long time', 'long lines',
            'away lines', 'at middle'), v[i:i + 8])); i += 8
    r['reached middle'], r['reached end'] = v[i:i + 2]
    return r

frames = [decode(v) for _, v in sorted(records.items())]
frames = [r for r in frames if first <= r['frame'] <= last]
if not frames:
    sys.exit('no frames in that range')
print(f'{len(frames)} frames, {frames[0]["frame"]} to {frames[-1]["frame"]}')

# Slow as the heartbeat log counts it: over 16 ms without the display wait, or over 17.5 ms with it
busy = lambda r: r['wall'] - r['slots']['EP_DISPLAY']
slow = [r for r in frames if busy(r) > 16000 or r['wall'] > 17500]
joined = lambda r: r['slots']['EP_JOIN2D_A'] + r['slots']['EP_JOIN2D_B']
groups = [('all', frames), ('slow, working', [r for r in slow if joined(r) < 500]),
    ('slow, waiting for 2D', [r for r in slow if joined(r) >= 500])]
groups = [(name, group) for name, group in groups if group]
mean = lambda group, get: sum(get(r) for r in group) / len(group)

def table(title, rows):
    # One line per value with a column per group, leaving out values that are small in every group
    print(f'\n{title:30}' + ''.join(f'{name:>22}' for name, _ in groups))
    for label, get in rows:
        values = [mean(group, get) for _, group in groups]
        if max(values) >= 1:
            print(f'{label:30}' + ''.join(f'{value:22.0f}' for value in values))

print('\n' + '  '.join(f'{name}: {len(group)}' for name, group in groups))
table('frame', [('wall', lambda r: r['wall']), ('without display wait', busy),
    ('reached line 96', lambda r: r['reached middle']), ('reached line 192', lambda r: r['reached end']),
    ('gpu build', lambda r: r['gpu build']), ('gpu draw', lambda r: r['gpu draw']),
    ('2D waits 3D', lambda r: r['2D waits 3D']), ('ring level', lambda r: r['ring level'])])
table('slots (nested, see emu_profile.h)', [(s[3:].lower(), lambda r, s=s: r['slots'][s]) for s in slots])
table('tasks', sorted([(t.lower(), lambda r, t=t: r['tasks'][t]) for t in tasks],
    key=lambda row: -mean(frames, row[1])))
table('counts', [(c[3:].lower(), lambda r, c=c: r['counts'][c]) for c in counters])
for engine in 'AB':
    table(f'engine {engine} 2D', [(k, lambda r, k=k, e=engine: r['2D ' + e][k]) for k in frames[0]['2D ' + engine]])
