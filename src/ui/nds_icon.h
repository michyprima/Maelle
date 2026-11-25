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
#include <string>

class NdsIcon {
public:
    NdsIcon(std::string path, int fd = -1);
    NdsIcon(const char *path): NdsIcon(path, -1) {}
    uint32_t *getIcon() { return icon; }
    const std::u16string &getTitle() { return title; } // The English banner title, with lines split by newlines
    const std::string &getGameCode() { return gameCode; } // Four letters, the last for the region
    uint8_t getUnitCode() { return unitCode; } // 0 for DS, 2 for DSi enhanced, 3 for DSi only
    uint8_t getRevision() { return revision; }
    uint32_t getTitleKind() { return titleKind; } // High half of the DSi title ID, like 0x00030004 for DSiWare, or 0

private:
    uint32_t icon[32 * 32];
    std::u16string title;
    std::string gameCode;
    uint8_t unitCode = 0, revision = 0;
    uint32_t titleKind = 0;
};
