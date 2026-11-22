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

class Input {
public:
    Input(Core *core): core(core) {}
    void saveState(FILE *file);
    void loadState(FILE *file, uint32_t version);

    void pressKey(int key);
    void releaseKey(int key);
    void pressScreen();
    void releaseScreen();
    void update();

    uint16_t readKeyInput() {
#ifdef FRAME_LOG
        if (watchKeys && (keyInput & watchKeys) != watchKeys) noteKeyRead();
#endif
        return keyInput;
    }
    uint16_t readExtKeyIn() {
#ifdef FRAME_LOG
        if (watchPen && !(extKeyIn & BIT(6))) noteKeyRead();
#endif
        return extKeyIn;
    }
    uint16_t readKeyCnt(int cpu) { return keyCnt[cpu]; }
    void writeKeyCnt(int cpu, uint16_t mask, uint16_t value);

#ifdef FRAME_LOG
    // Pressed keys the frame log waits for the game to read, and when it first did, for measuring input lag
    volatile uint16_t watchKeys = 0;
    volatile bool watchPen = false;
    volatile uint32_t readTick = 0, readFrame = 0, readCount = 0;
    // Keys newly pressed when the emulator thread reads the controllers itself, for the UI to log
    volatile uint32_t pressTick = 0, pressFrame = 0, pressKeys = 0, pressCount = 0;
#endif

private:
    Core *core;

#ifdef FRAME_LOG
    void noteKeyRead();
#endif

    uint16_t keyInput = 0x03FF;
    uint16_t extKeyIn = 0x007F;
    uint16_t keyCnt[2] = {};
    uint16_t lastKeys = 0x03FF;
};
