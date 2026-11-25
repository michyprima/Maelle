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
#include <unordered_map>
#include <vector>

// Menu text drawn with a TrueType font, like the Wii U's system font, whose letters are drawn the first time they're
// needed into an atlas texture and reused from there
// Sizes are those of the old bitmap font's square cells: a font at cell size s has its capitals 25/48 of s tall with
// the baseline 36/48 of s down, so text sits where it always did, and widths are given at cell size 48
class MenuFont {
public:
    static const int ATLAS_WIDTH = 2048, ATLAS_HEIGHT = 1024;

    struct Glyph {
        int x, y, w, h; // Where the letter is in the atlas, if it has any pixels
        int left, top; // Where its pixels start from the pen and the baseline
    };

    // Writes pixels to the atlas, white with the letter's coverage as alpha in the UI's texture layout
    typedef void (*Upload)(void *user, const uint32_t *data, int x, int y, int w, int h);

    MenuFont(): info(nullptr) {}
    ~MenuFont();
    bool load(const uint8_t *data, size_t size, Upload upload, void *user);
    bool loaded() const { return info; }

    // Read one character from UTF-8, where a lone byte that isn't UTF-8 is taken as itself, like the button symbols
    static uint32_t next(const std::string &text, size_t &i);

    float advance(uint32_t c); // At cell size 48, with no kerning
    float kerning(uint32_t a, uint32_t b); // At cell size 48
    const Glyph &glyph(uint32_t c, int size);
    int resets() const { return resetCount; }

private:
    void *info; // stbtt_fontinfo
    float unitScale; // Cell size 48 per font unit
    Upload upload;
    void *user;

    std::unordered_map<uint32_t, int> indices; // Glyph index for each character
    std::unordered_map<uint32_t, float> advances;
    std::unordered_map<uint64_t, Glyph> glyphs; // By character and size
    int shelfX = 0, shelfY = 0, shelfHeight = 0;
    int resetCount = 0;
    std::vector<uint8_t> coverage;
    std::vector<uint32_t> pixels;

    int index(uint32_t c);
};
