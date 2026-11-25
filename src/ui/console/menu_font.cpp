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

#include <algorithm>

#include "menu_font.h"

// stb_truetype (v1.27, public domain or MIT, github.com/nothings/stb) turns the font's outlines into pixels
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define INFO ((stbtt_fontinfo*)info)

MenuFont::~MenuFont() {
    delete INFO;
}

bool MenuFont::load(const uint8_t *data, size_t size, Upload upload, void *user) {
    // Read the font's tables, keeping the data where it is, which has to stay until the font is done with
    stbtt_fontinfo *font = new stbtt_fontinfo;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (!size || offset < 0 || !stbtt_InitFont(font, data, offset)) {
        delete font;
        return false;
    }

    // Size the font by its capital H, which is 25 pixels tall at cell size 48 like the old font's, or by 70% of its
    // ascent if it has no H
    int x0, y0, x1, y1, ascent, descent, gap;
    int cap = stbtt_GetCodepointBox(font, 'H', &x0, &y0, &x1, &y1) ? y1 : 0;
    stbtt_GetFontVMetrics(font, &ascent, &descent, &gap);
    if (cap <= 0) cap = ascent * 7 / 10;
    if (cap <= 0) {
        delete font;
        return false;
    }
    info = font;
    unitScale = 25.0f / cap;
    this->upload = upload;
    this->user = user;
    return true;
}

uint32_t MenuFont::next(const std::string &text, size_t &i) {
    // Decode a UTF-8 character, taking any byte that doesn't start a whole one as itself
    uint8_t c = text[i++];
    int extra = (c >= 0xF0 && c < 0xF8) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (c < 0xC0 || i + extra > text.size()) return c;
    uint32_t value = c & (0x3F >> extra);
    for (int j = 0; j < extra; j++) {
        uint8_t byte = text[i + j];
        if ((byte & 0xC0) != 0x80) return c;
        value = (value << 6) | (byte & 0x3F);
    }
    i += extra;
    return value;
}

int MenuFont::index(uint32_t c) {
    // Look up a character's glyph once, which is the font's missing letter box if it has none
    auto it = indices.find(c);
    if (it != indices.end()) return it->second;
    return indices[c] = stbtt_FindGlyphIndex(INFO, c);
}

float MenuFont::advance(uint32_t c) {
    // Give how far the pen moves after a character at cell size 48
    auto it = advances.find(c);
    if (it != advances.end()) return it->second;
    int advance, bearing;
    stbtt_GetGlyphHMetrics(INFO, index(c), &advance, &bearing);
    return advances[c] = advance * unitScale;
}

float MenuFont::kerning(uint32_t a, uint32_t b) {
    // Give the font's spacing change between two characters at cell size 48
    return stbtt_GetGlyphKernAdvance(INFO, index(a), index(b)) * unitScale;
}

const MenuFont::Glyph &MenuFont::glyph(uint32_t c, int size) {
    // Use the letter if it's already drawn at this size
    uint64_t key = (uint64_t(c) << 16) | uint16_t(size);
    auto it = glyphs.find(key);
    if (it != glyphs.end()) return it->second;

    // Measure the letter's pixels at this size
    float scale = unitScale * size / 48;
    int gi = index(c), x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(INFO, gi, scale, scale, &x0, &y0, &x1, &y1);
    Glyph glyph = { 0, 0, x1 - x0, y1 - y0, x0, y0 };
    if (glyph.w <= 0 || glyph.h <= 0 || glyph.w >= ATLAS_WIDTH || glyph.h >= ATLAS_HEIGHT) {
        // Spaces and anything too big have nothing to draw
        glyph.w = glyph.h = 0;
        return glyphs[key] = glyph;
    }

    // Find room in the atlas on a row of letters, a pixel apart so filtering doesn't pick up their neighbours, and
    // start the atlas over if it's full
    if (shelfX + glyph.w + 1 > ATLAS_WIDTH) {
        shelfY += shelfHeight;
        shelfX = shelfHeight = 0;
    }
    if (shelfY + glyph.h + 1 > ATLAS_HEIGHT) {
        glyphs.clear();
        shelfX = shelfY = shelfHeight = 0;
        resetCount++;
    }
    glyph.x = shelfX;
    glyph.y = shelfY;
    shelfX += glyph.w + 1;
    shelfHeight = std::max(shelfHeight, glyph.h + 1);

    // Draw the letter and write it to the atlas as white with its coverage as alpha
    coverage.assign(glyph.w * glyph.h, 0);
    stbtt_MakeGlyphBitmap(INFO, coverage.data(), glyph.w, glyph.h, glyph.w, scale, scale, gi);
    pixels.resize(coverage.size());
    for (size_t i = 0; i < coverage.size(); i++)
        pixels[i] = (uint32_t(coverage[i]) << 24) | 0xFFFFFF;
    upload(user, pixels.data(), glyph.x, glyph.y, glyph.w, glyph.h);
    return glyphs[key] = glyph;
}
