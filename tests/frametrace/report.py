#!/usr/bin/env python3
# Sums up the trace.bin that a `./build-wiiu-debug.sh frameslots` build of Maelle writes to its folder on the SD card
# next to heartbeat.log: every emulated frame's record of where the emulator thread spent its time, by profiling slot and
# by scheduled task, how the 2D engines drew the frame and how long the GPU took over its 3D. Frames are grouped into all
# of them, slow ones where the emulator thread worked, and slow ones where it waited for a 2D engine at the end of the
# frame, since saving time on the emulator thread only helps the first kind. `frametime` builds write the same records
# for slow frames and every 60th, along with samples of where the emulator's core was 5,000 times a second during them;
# given that build's .elf, those are shown as the emulator's functions, the game's code the JIT compiled (named by its
# ARM9 or ARM7 address, from the log of compiled blocks), and the emulator's functions that code called
# Frames end at line 192, so a record's wall time is the display wait and the previous frame's V-blank lines, then this
# frame's lines up to 192 (reached line 192 counts from line 0) and the wait for the 2D engines to finish
# Usage: report.py <trace.bin> [--from frame] [--to frame] [--elf build.elf] [--top N]
import bisect, collections, os, re, struct, subprocess, sys

args = sys.argv[1:]
first, last, elf, top = 0, 1 << 32, None, 25
for flag in ('--from', '--to', '--elf', '--top'):
    if flag in args:
        at = args.index(flag)
        value = args[at + 1]
        if flag == '--from': first = int(value)
        elif flag == '--to': last = int(value)
        elif flag == '--elf': elf = value
        else: top = int(value)
        del args[at:at + 2]
if len(args) != 1:
    sys.exit('Usage: report.py <trace.bin> [--from frame] [--to frame] [--elf build.elf] [--top N]')

# The names of the profiling slots, counts and tasks, in the order the core numbers them
core = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../src/core')
def enum(path, name):
    text = open(os.path.join(core, path)).read()
    body = re.search(r'enum %s \{(.*?)\};' % name, text, re.S).group(1)
    return [m.group(1) for m in re.finditer(r'^\s*([A-Z0-9_]+)\s*(?:,|$)', body, re.M) if not m.group(1).endswith('COUNT')]
slots = enum('emu_profile.h', 'EmuProfileSlot')
counters = enum('emu_profile.h', 'EmuCountSlot')
tasks = enum('core.h', 'SchedTask')[:-1]

# A header of ten big-endian words: a tag, the version, where FrameTrace::write was loaded, the codegen area's start and
# size, the timer speed, the slot and count totals, and the sizes of records and samples in words. Then chunks of a tag,
# a size in words and the words: FRAM holds a record's index and the record, SMPS samples of the frames whose records
# came before, BLKS the JIT's log of compiled blocks and stubs, LOST a gap in that log, and RSMP the GPU renderer's
# samples
data = open(args[0], 'rb').read()
header = struct.unpack('>10I', data[:40])
if header[0] != 0x4E545243:
    sys.exit(f'{args[0]} is not a frame trace')
writeAddress, codegen, codegenEnd = header[2], header[3], header[3] + header[4]
epCount, ecCount, recordWords, sampleWords = header[6], header[7], header[8], header[9]
if (epCount, ecCount) != (len(slots), len(counters)):
    sys.exit(f'the trace has {epCount} slots and {ecCount} counts, the source has {len(slots)} and {len(counters)}')
if recordWords != 2 + epCount + ecCount + 6 + 64 + 16 + 2:
    sys.exit(f'the trace\'s records are {recordWords} words, which this script doesn\'t know')
if sampleWords != 8:
    sys.exit(f'the trace\'s samples are {sampleWords} words, which this script doesn\'t know')

# The JIT's compiled blocks and stubs, by where their code starts: its end, whether it's the ARM7's, the guest address
# with bit 0 set for THUMB, how many instructions, and a stub's name; the log is followed in file order, so samples see
# the blocks of their time
blocks, starts, lost = {}, None, 0
def findBlock(address):
    global starts
    if starts is None: starts = sorted(blocks)
    i = bisect.bisect_right(starts, address) - 1
    if i >= 0 and address < blocks[starts[i]][0]: return blocks[starts[i]]
    return None

# Each sample is the record index it falls in, the interrupted address and link register, four return addresses from
# the stack, and the last block entered, kept with the block found for the address and the first found up the stack.
# The record index has its top bit set when the emulator's thread wasn't running, and then the link register and
# return addresses are where that thread was stopped
records, samples, at = {}, [], 40
while at + 8 <= len(data):
    tag, words = data[at:at + 4], struct.unpack('>I', data[at + 4:at + 8])[0]
    values = struct.unpack('>%dI' % words, data[at + 8:at + 8 + words * 4])
    if tag == b'FRAM':
        records[values[0]] = values[1:]
    elif tag == b'BLKS':
        i = 0
        while i + 5 <= len(values):
            kind, arm7, count = values[i] >> 24, (values[i] >> 16) & 0xFF, values[i] & 0xFFFF
            key, start, end = values[i + 2:i + 5]
            if kind == 1:
                blocks[start] = (end, arm7, key, count, None)
            elif kind == 3:
                name = struct.pack('>%dI' % count, *values[i + 5:i + 5 + count]).split(b'\0')[0].decode()
                blocks[start] = (end, arm7, 0, 0, name)
            elif kind == 2:
                for address in [a for a, b in blocks.items() if start <= a < end and b[1] == arm7]:
                    del blocks[address]
            starts = None
            i += 5 + count
    elif tag == b'LOST':
        lost += 1
    elif tag == b'SMPS':
        for i in range(0, len(values) - 7, 8):
            sample = values[i:i + 8]
            stack = (sample[2],) + sample[3:7]
            if sample[0] >> 31:
                samples.append((sample[0] & 0x7FFFFFFF, sample[1], stack, None, None, False))
                continue
            samples.append((sample[0], sample[1], stack, findBlock(sample[1]) if codegen <= sample[1] < codegenEnd
                else None, next((findBlock(a) for a in stack if codegen <= a < codegenEnd), None), True))
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

if not samples:
    sys.exit(0)

# The emulator's functions, from the .elf, moved to where the console loaded them
symbols, symbolStarts = [], []
if elf:
    tools = os.environ.get('DEVKITPPC', os.environ.get('DEVKITPRO', '/opt/devkitpro') + '/devkitPPC') + '/bin/powerpc-eabi-'
    for line in subprocess.run([tools + 'nm', '-S', '-C', '--defined-only', elf], capture_output=True,
            text=True).stdout.splitlines():
        parts = line.split(' ', 3)
        if len(parts) == 4 and parts[2] in 'tTwW':
            symbols.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
    symbols.sort()
    write = [s for s in symbols if s[2].startswith('FrameTrace::write(')]
    if not write:
        sys.exit(f'{elf} has no FrameTrace::write, so it isn\'t a frametime build')
    offset = writeAddress - write[0][0]
    symbols = [(start + offset, size, name) for start, size, name in symbols]
    symbolStarts = [s[0] for s in symbols]

def function(address):
    # Name an address outside generated code, cutting C++ argument lists short
    i = bisect.bisect_right(symbolStarts, address) - 1
    if i >= 0 and address < symbols[i][0] + max(symbols[i][1], 4):
        name = symbols[i][2]
        return name[:name.index('(')] if '(' in name else name
    return '<system or unknown %07Xx>' % (address >> 4) if symbols else '<%07Xx>' % (address >> 4)

def guest(block):
    # Name a compiled block by its CPU and guest address, or a stub by its name
    if block[4]: return '%s stub: %s' % ('ARM7' if block[1] else 'ARM9', block[4])
    return '%s %08X%s' % ('ARM7' if block[1] else 'ARM9', block[2] & ~1, ' THUMB' if block[2] & 1 else '')

def stopped(stack):
    # Where a thread that isn't running was stopped: the first two of the emulator's functions up its stack
    found = [function(a) for a in stack if a and not function(a).startswith('<')]
    return ' <- '.join(found[:2]) if found else '<only system code>'

# Each sample counts in the groups of the frame it fell in, as 200 microseconds of that frame
groupOf = collections.defaultdict(list)
byIndex = {index: decode(v) for index, v in records.items()}
for index, r in byIndex.items():
    if not first <= r['frame'] <= last: continue
    slowFrame = busy(r) > 16000 or r['wall'] > 17500
    names = ['all'] + (['slow, working' if joined(r) < 500 else 'slow, waiting for 2D'] if slowFrame else [])
    groupOf[index] = names
names = [name for name, _ in groups]
sampled = {name: len({index for index, _, _, _, _, _ in samples if name in groupOf.get(index, ())}) for name in names}
print('\nsamples: %d, from %d frames; %d gaps in the block log%s' % (len(samples), len({s[0] for s in samples}), lost,
    '' if elf else '; give --elf for function names'))
print('Times are ms per sampled frame in each group: ' + ', '.join('%s %d' % (n, sampled[n]) for n in names))

def tally(key):
    # Count each sample under what key gives it, per group
    counts = collections.defaultdict(lambda: collections.Counter())
    for index, pc, stack, block, caller, running in samples:
        label = key(pc, stack, block, caller) if running else key.stopped(pc, stack) if hasattr(key, 'stopped') else None
        if label is None: continue
        for name in groupOf.get(index, ()):
            counts[label][name] += 1
    return counts

def show(title, counts, limit):
    # A line per label with ms per frame for each group, the most in slow frames where the emulator worked first
    print(f'\n{title[:58]:58}' + ''.join(f'{n[:20]:>21}' for n in names))
    rank = 'slow, working' if sampled.get('slow, working') else names[0]
    order = sorted(counts, key=lambda label: -counts[label][rank] - counts[label]['all'] / 1e6)
    for label in order[:limit]:
        print(f'{label[:58]:58}' + ''.join(f'{counts[label][n] * 0.2 / max(sampled[n], 1):21.2f}' for n in names))

def kind(pc, stack, block, caller):
    if block and block[4]: return 'JIT stubs, ' + ('ARM7' if block[1] else 'ARM9')
    if block: return 'game code, ' + ('ARM7' if block[1] else 'ARM9')
    if codegen <= pc < codegenEnd: return 'generated code not in the block log'
    if caller: return 'emulator called from game code, ' + ('ARM7' if caller[1] else 'ARM9')
    name = function(pc)
    return 'system or unknown' if name.startswith('<') else 'emulator, elsewhere'
kind.stopped = lambda pc, stack: 'emulator thread not running: ' + (
    'core idle or in the system' if function(pc).startswith('<') else 'another thread')
show('where the core was', tally(kind), 12)
def stoppedAt(pc, stack, block, caller): return None
stoppedAt.stopped = lambda pc, stack: stopped(stack)
show('where the emulator thread was while it wasn\'t running', tally(stoppedAt), top)
def others(pc, stack, block, caller): return None
others.stopped = lambda pc, stack: function(pc)
show('what ran on the core instead', tally(others), 10)
show('emulator functions called from game code', tally(lambda pc, stack, block, caller:
    function(pc) if caller and not block and not codegen <= pc < codegenEnd else None), top)
show('emulator functions elsewhere', tally(lambda pc, stack, block, caller:
    function(pc) if not caller and not codegen <= pc < codegenEnd else None), top)
show('JIT stubs', tally(lambda pc, stack, block, caller:
    guest(block) if block and block[4] else None), 15)
show('game code, by block (inside it)', tally(lambda pc, stack, block, caller:
    guest(block) if block and not block[4] else None), top)
show('game code, by block (with the emulator functions it called)', tally(lambda pc, stack, block, caller:
    guest(block) if block else guest(caller) if caller else None), top)
