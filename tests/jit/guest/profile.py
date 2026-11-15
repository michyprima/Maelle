#!/usr/bin/env python3
# Sums up what GUEST_PROFILE counted, naming addresses by the functions in the guest program, see README.md
# Usage: profile.py <guest.elf> <profile> <frames counted> [functions [N] | callers | annotate <function or address>]
# functions lists the N functions that ran the most instructions (40 by default), callers lists where calls to
# GUEST_CALLERS's functions came from, and annotate shows a function's instructions with their counts, and its source lines
# by how many instructions each ran; every count is per frame
import bisect, collections, os, re, subprocess, sys

elf, path, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
mode = sys.argv[4] if len(sys.argv) > 4 else 'functions'
tools = os.environ.get('DEVKITPPC', os.environ.get('DEVKITPRO', '/opt/devkitpro') + '/devkitPPC') + '/bin/powerpc-eabi-'
CODEGEN, CODEGEN_END = 0x00100000, 0x02100000 # From layout.h

# The program's functions, by start address
symbols = []
for line in subprocess.run([tools + 'nm', '-S', '-C', '--defined-only', elf], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(' ', 3)
    if len(parts) == 4 and parts[2] in 'tTwW':
        symbols.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
symbols.sort()
starts = [s[0] for s in symbols]

def name(address):
    if CODEGEN <= address < CODEGEN_END: return '<generated code>'
    i = bisect.bisect_right(starts, address) - 1
    if i >= 0 and address < symbols[i][0] + max(symbols[i][1], 4): return symbols[i][2]
    return '?%08X' % address

# The counts, then the callers of each function GUEST_CALLERS gave
counts, callers, callee = {}, collections.defaultdict(collections.Counter), None
for line in open(path):
    parts = line.split()
    if parts[0] == 'callee': callee = int(parts[1], 16)
    elif parts[0] == 'caller': callers[callee][int(parts[1], 16)] += int(parts[2])
    else: counts[int(parts[0], 16)] = int(parts[1])

if mode == 'functions':
    top = int(sys.argv[5]) if len(sys.argv) > 5 else 40
    total = sum(counts.values())
    byName = collections.Counter()
    for address, count in counts.items(): byName[name(address)] += count
    print('%.2fM instructions per frame' % (total / frames / 1e6))
    for function, count in byName.most_common(top):
        print('%9.0fK %5.1f%%  %s' % (count / frames / 1e3, 100 * count / total, function))

elif mode == 'callers':
    for function, fromAddresses in callers.items():
        print('%s: %.0f calls per frame' % (name(function), sum(fromAddresses.values()) / frames))
        for address, count in fromAddresses.most_common(10):
            print('   %9.0f from %08X in %s' % (count / frames, address, name(address)))

elif mode == 'annotate':
    target = sys.argv[5]
    match = [s for s in symbols if s[2] == target or s[2].startswith(target + '(') or '%08x' % s[0] == target.lower().zfill(8)]
    if not match: sys.exit('no function ' + target)
    start, size, function = match[0]
    out = subprocess.run([tools + 'objdump', '-d', '-l', '--no-show-raw-insn', '--start-address=%#x' % start,
        '--stop-address=%#x' % (start + size), elf], capture_output=True, text=True).stdout
    source, bySource = None, collections.Counter()
    print(function)
    for line in out.splitlines():
        instruction = re.match(r'^\s*([0-9a-f]+):\s*(.*)', line)
        if instruction:
            count = counts.get(int(instruction.group(1), 16), 0) / frames
            bySource[source] += count
            print('%9.0f  %s' % (count, line.strip()) if count else '           ' + line.strip())
        elif re.match(r'^/.*:\d+', line):
            source = line.strip().split('/')[-1]
    print('\nBy source line:')
    for line, count in bySource.most_common(30):
        if count: print('%9.0f  %s' % (count, line))

else:
    sys.exit('unknown mode ' + mode)
