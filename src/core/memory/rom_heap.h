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

#pragma once

#include <cstddef>
#include <cstdint>

// On Wii U, ROMs kept in RAM are taken from the top of malloc's memory, which grows up from the bottom, so what's
// allocated while a game runs never ends up above a ROM; freeing a ROM then leaves one free block for the next one
uint8_t *allocTopRom(size_t size);
bool freeTopRom(void *rom);

// The memory malloc can still give out: what's free inside its memory, and between it and the lowest ROM
size_t freeHeapBytes();
