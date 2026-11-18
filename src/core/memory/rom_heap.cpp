/*
    Copyright 2026 michyprima

    This file is part of Maelle.

    Maelle is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Maelle is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Maelle. If not, see <https://www.gnu.org/licenses/>.
*/

#ifdef __WIIU__

#include <algorithm>
#include <cerrno>
#include <malloc.h>
#include <coreinit/memexpheap.h>

#include "rom_heap.h"

// These replace wut's own sbrk heap, which takes all memory left at start for malloc and grows it up from the bottom
// Here sbrk stops below the lowest ROM, and ROMs are placed from the top down; both happen under malloc's lock

struct TopRom {
    uint32_t offset, size;
};

static MEMHeapHandle heapHandle = nullptr;
static uint8_t *heapBase = nullptr;
static uint32_t heapSize = 0;
static uint32_t heapBreak = 0;
static uint32_t romLow = 0;
static TopRom topRoms[8];
static int topRomCount = 0;

extern "C" void __init_wut_sbrk_heap(MEMHeapHandle heap) {
    // Take all memory left in the heap, as wut does
    if (heapBase) return;
    heapHandle = heap;
    heapSize = MEMGetAllocatableSizeForExpHeapEx(heap, 4);
    heapBase = (uint8_t*)MEMAllocFromExpHeapEx(heap, heapSize, 4);
    heapBreak = 0;
    romLow = heapSize;
}

extern "C" void __fini_wut_sbrk_heap() {
    if (!heapBase) return;
    if (heapHandle) MEMFreeToExpHeap(heapHandle, heapBase);
    heapBase = nullptr;
    heapSize = 0;
}

extern "C" void *__wut_sbrk_r(struct _reent *r, ptrdiff_t incr) {
    // Grow or shrink malloc's memory, which can't reach the ROMs above it
    // Only malloc calls this, holding its lock
    uint32_t old = heapBreak;
    if ((incr > 0 && uint32_t(incr) > romLow - old) || (incr < 0 && uint32_t(-incr) > old)) {
        r->_errno = ENOMEM;
        return (void*)-1;
    }
    heapBreak = old + incr;
    return heapBase + old;
}

static void updateRomLow() {
    romLow = heapSize;
    for (int i = 0; i < topRomCount; i++)
        romLow = std::min(romLow, topRoms[i].offset);
}

uint8_t *allocTopRom(size_t size) {
    // Find the highest gap that fits, going down from the top past each ROM, with 64-byte aligned starts
    if (!heapBase || size == 0 || size > heapSize) return nullptr;
    __malloc_lock(_REENT);
    uint8_t *rom = nullptr;
    if (topRomCount < 8) {
        std::sort(topRoms, topRoms + topRomCount, [](const TopRom &a, const TopRom &b) { return a.offset > b.offset; });
        uint32_t end = heapSize;
        for (int i = 0; i <= topRomCount; i++) {
            uint32_t low = (i < topRomCount) ? (topRoms[i].offset + topRoms[i].size) : heapBreak;
            if (end >= low + size) {
                uint32_t start = uint32_t((uintptr_t(heapBase) + end - size) & ~uintptr_t(63)) - uintptr_t(heapBase);
                if (start >= low) {
                    topRoms[topRomCount++] = { start, uint32_t(size) };
                    updateRomLow();
                    rom = heapBase + start;
                    break;
                }
            }
            if (i < topRomCount) end = topRoms[i].offset;
        }
    }
    __malloc_unlock(_REENT);
    return rom;
}

bool freeTopRom(void *rom) {
    // Give a ROM's memory back if it came from the top
    if (!rom) return false;
    __malloc_lock(_REENT);
    bool found = false;
    for (int i = 0; i < topRomCount; i++) {
        if (heapBase + topRoms[i].offset != rom) continue;
        topRoms[i] = topRoms[--topRomCount];
        updateRomLow();
        found = true;
        break;
    }
    __malloc_unlock(_REENT);
    return found;
}

#endif // __WIIU__
