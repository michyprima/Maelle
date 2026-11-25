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
#include <string>
#include <vector>

// Facts from ROM headers, shown as pills under games in the file browser
namespace RomTags {
    std::string region(char letter); // A DS or GBA game code's last letter, as GBATEK lists them, or nothing
    std::string size(uint64_t bytes);
    std::vector<std::string> gba(const uint8_t *header, size_t length); // From the start of the ROM, 0xC0 bytes
    std::vector<std::string> gb(const uint8_t *header, size_t length); // From the start of the ROM, 0x150 bytes
}
