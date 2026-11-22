/*
    Copyright 2019-2026 Hydr8gon

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

#pragma once

#include <cstdint>
#include <cstdio>

class Core;

class Cp15 {
    friend class Jit;
public:
    uint32_t exceptionAddr = 0;
    bool dtcmCanRead = false, dtcmCanWrite = false;
    bool itcmCanRead = false, itcmCanWrite = false;
    uint32_t dtcmAddr = 0, dtcmSize = 0;
    uint32_t itcmSize = 0;

    // The ARM9's code cache as far as timing goes, which is only counted for main RAM: 64 sets of four 32-byte lines,
    // where a line that isn't there takes the place of the next of its set by turns, as measured on a DSi with
    // tests/hardware's speed test
    // Each place holds its line plus 1, or 0 for none, MSB-first so generated code reads it as a word on any host
    // That number is also where the byte is that's set while the cache holds the line, so the byte for no line can be
    // cleared like any other when a place is given to another line; the places come first to be in reach of the bytes
    static const int CODE_SETS = 64, CODE_WAYS = 4;
    static const int MISS_TICKS = 22; // What fetching a line takes, in 33MHz cycles
    bool codeCache = false;
    uint8_t codeTags[CODE_SETS * CODE_WAYS * 4] = {};
    uint8_t codeTurn = 0; // Which of its set's lines the next one fetched replaces
    uint8_t codeLines[(0x1000000 >> 5) + 1] = {};

    Cp15(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file, uint32_t version);

    uint32_t read(uint8_t cn, uint8_t cm, uint8_t cp);
    void write(uint8_t cn, uint8_t cm, uint8_t cp, uint32_t value);

    bool codeMissing(uint32_t address) { return (address >> 24) == 0x02 && !codeLines[((address & 0xFFFFFF) >> 5) + 1]; }
    int codeMiss(uint32_t address);
    void codeFlush();

private:
    Core *core;
    uint32_t ctrlReg = 0x78;
    uint32_t dtcmReg = 0x00;
    uint32_t itcmReg = 0x00;
    uint32_t procId = 0x00;

    uint32_t codeTag(uint32_t at) {
        uint8_t *tag = &codeTags[at * 4];
        return (tag[0] << 24) | (tag[1] << 16) | (tag[2] << 8) | tag[3];
    }
    void setCodeTag(uint32_t at, uint32_t value) {
        uint8_t *tag = &codeTags[at * 4];
        tag[0] = value >> 24, tag[1] = value >> 16, tag[2] = value >> 8, tag[3] = value;
    }
    void codeForget(uint32_t at);
};
