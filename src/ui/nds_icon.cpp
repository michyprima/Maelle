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

#include <cstring>

#include "nds_icon.h"
#include "../core/defines.h"

NdsIcon::NdsIcon(std::string path, int fd) {
    // Attempt to open the ROM
    FILE *rom = (fd == -1) ? fopen(path.c_str(), "rb") : fdopen(fd, "rb");

    // Create an empty icon if ROM loading failed
    if (!rom) {
        LOG_WARN("Failed to open ROM for icon decoding!\n");
        memset(icon, 0, 32 * 32 * sizeof(uint32_t));
        return;
    }

    // Get the header up to the DSi title ID, with the game code and the icon offset, all in the SD's first block
    uint8_t header[0x238] = {};
    fread(header, sizeof(uint8_t), sizeof(header), rom);
    gameCode = std::string((char*)&header[0x0C], 4);
    unitCode = header[0x12];
    revision = header[0x1E];
    if (unitCode & 2) titleKind = U8TO32(header, 0x234);
    uint8_t *offset = &header[0x68];

    // Get the banner's icon, palette and the titles up to English in one read, since SD reads are slow
    uint8_t banner[0x440] = {};
    fseek(rom, U8TO32(offset, 0), SEEK_SET);
    fread(banner, sizeof(uint8_t), sizeof(banner), rom);
    fclose(rom);
    uint8_t *data = &banner[0x20];
    uint8_t *palette = &banner[0x220];

    // Get the English title, 128 UTF-16 characters at most, if the ROM has a banner
    if (U8TO32(offset, 0)) {
        for (int i = 0; i < 128; i++) {
            char16_t c = U8TO16(banner, 0x340 + i * 2);
            if (!c) break;
            title += c;
        }
    }

    // Get each pixel's 5-bit palette color and convert it to 8-bit
    uint32_t tiles[32 * 32];
    for (int i = 0; i < 32 * 32; i++) {
        uint8_t index = (i & 1) ? ((data[i / 2] & 0xF0) >> 4) : (data[i / 2] & 0x0F);
        uint16_t color = index ? U8TO16(palette, index * 2) : 0xFFFF;
        uint8_t r = ((color >> 0) & 0x1F) * 255 / 31;
        uint8_t g = ((color >> 5) & 0x1F) * 255 / 31;
        uint8_t b = ((color >> 10) & 0x1F) * 255 / 31;
        tiles[i] = (0xFF << 24) | (b << 16) | (g << 8) | r;
    }

    // Rearrange the pixels from 8x8 tiles to a 32x32 icon
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            for (int k = 0; k < 4; k++)
                memcpy(&icon[256 * i + 32 * j + 8 * k], &tiles[256 * i + 8 * j + 64 * k], 8 * sizeof(uint32_t));
        }
    }
}
