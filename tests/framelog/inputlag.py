#!/usr/bin/env python3
# Copyright 2026 michyprima
#
# Input lag from a FRAME_LOG build's framelog.bin: for each new press of a KEYINPUT key (A, B, Select, Start, the
# D-pad, L, R) or of the touch screen, when it was sent to the core, when the game first read it, and when the frame
# that could first show it reached the display (its swap's flip). What the game itself does with the key afterwards is
# the same as on a DS. Also counts the UI's repeated and skipped frames, since cutting lag mustn't cost smoothness.
#
# Usage: inputlag.py framelog.bin

import struct
import sys
from statistics import mean, median

TYPE_UI, TYPE_PRESS, TYPE_READ, TYPE_SWAP, TYPE_INPUT_COST = 3, 11, 12, 13, 14


def delta(a, b):
    # Ticks from b to a, for the 32-bit tick counter that wraps every minute or so
    d = (a - b) & 0xFFFFFFFF
    return d - (1 << 32) if d & 0x80000000 else d


def main():
    data = open(sys.argv[1], 'rb').read()
    entries = [struct.unpack('>4I', data[i:i + 16]) for i in range(0, len(data) - 15, 16)]
    if not entries or entries[0][0] != 0x4E464C47:
        sys.exit('not a frame log')
    ms = 1000.0 / entries[0][1]

    # Walk the UI's entries in order: each UI frame got a new frame or not, maybe sent a press, then swapped
    shown = 0  # ID of the frame the UI shows
    swaps = []  # Swap counts in order, each with the frame ID it put on the display
    flipTimes = {}  # Flip count -> when it happened
    presses = []  # [press time, emulated frame at press, keys, read time, read frame, frames queued, swaps before]
    # Presses made on the emulator thread are logged by the UI a little later, so the frame shown at a press and the
    # press a read belongs to are found by time
    recent = []  # (time, frame ID shown from then) for the last UI frames
    for kind, time, value, extra in entries[1:]:
        if kind == TYPE_UI:
            if value:
                shown = value
            recent = (recent + [(time, shown)])[-8:]
        elif kind == TYPE_PRESS:
            before = [s for t, s in recent if delta(time, t) >= 0]
            presses.append([time, value, extra, None, None, value - (before[-1] if before else shown), len(swaps)])
        elif kind == TYPE_READ:
            for p in reversed(presses[-4:]):
                if p[3] is None and delta(time, p[0]) >= 0:
                    p[3], p[4] = time, value
                    break
        elif kind == TYPE_SWAP:
            swaps.append((value, shown))
            if extra not in flipTimes:
                flipTimes[extra] = time

    # The display time of a frame is the flip of the first swap showing it or a later one; flips the UI didn't see
    # happen a refresh apart from ones it did
    refresh = 1000.0 / 59.94 / ms
    def flipOf(swap):
        if swap in flipTimes:
            return flipTimes[swap]
        known = [s for s in flipTimes if s > swap]
        if not known:
            return None
        s = min(known)
        return (flipTimes[s] - round((s - swap) * refresh)) & 0xFFFFFFFF

    # Frame IDs start over when a game is loaded, so only the swaps from the press on are searched, for 2 seconds' worth
    def displayed(frame, start):
        for swap, frameId in swaps[start:start + 120]:
            if frameId >= frame:
                return flipOf(swap), frameId
        return None, None

    names = ['A', 'B', 'Sel', 'Sta', 'R>', '<L', 'Up', 'Dn', 'R', 'L', 'Tch']
    rows = []
    print('%5s %-6s %7s %7s %8s %6s %6s' % ('press', 'keys', 'read', 'shown', 'total', 'ahead', 'late'))
    for n, (tp, fp, keys, tr, fr, queued, start) in enumerate(presses):
        label = '+'.join(names[i] for i in range(11) if keys >> i & 1)
        if tr is None or delta(tr, tp) > 2000 / ms:
            print('%5d %-6s   (game didn\'t read it within 2 s)' % (n, label))
            continue
        flip, frame = displayed(fr, start)
        if flip is None:
            continue
        read = delta(tr, tp) * ms
        total = delta(flip, tp) * ms
        rows.append((read, total - read, total, queued, frame - fr))
        print('%5d %-6s %7.1f %7.1f %8.1f %6d %6d' % (n, label, read, total - read, total, queued, frame - fr))

    if rows:
        print()
        for i, name in enumerate(['press -> game read', 'game read -> on display', 'press -> on display']):
            values = [r[i] for r in rows]
            print('%-24s mean %5.1f  median %5.1f  min %5.1f  max %5.1f ms' %
                  (name, mean(values), median(values), min(values), max(values)))
        print('%d presses; frames emulated ahead of the display at press: mean %.2f; shown later than the read frame: '
              'mean %.2f' % (len(rows), mean(r[3] for r in rows), mean(r[4] for r in rows)))


    # Frames the UI had nothing new for, so it showed one twice, and frames it never showed
    ids = [e[2] for e in entries[1:] if e[0] == TYPE_UI]
    shownIds = [i for i in ids if i]
    skipped = sum(b - a - 1 for a, b in zip(shownIds, shownIds[1:]) if 0 < b - a < 1000)
    print('%d UI frames: %d repeated a frame, %d frames skipped' % (len(ids), ids.count(0), skipped))

    # What reading the controllers cost the emulator thread before each frame
    costs = sorted(e[2] * ms for e in entries[1:] if e[0] == TYPE_INPUT_COST)
    if costs:
        print('early controller read: mean %.3f  median %.3f  99%% under %.3f  max %.3f ms over %d frames' %
              (mean(costs), costs[len(costs) // 2], costs[min(len(costs) - 1, int(len(costs) * .99))], costs[-1],
               len(costs)))


main()
