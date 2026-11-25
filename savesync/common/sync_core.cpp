/*
    Copyright 2026 michyprima
*/

#include "sync_core.h"
#include <cstdio>
#include <map>

namespace SaveSync {

// CRC-16/MODBUS, as the DS header CRC at 0x15E is computed over 0x000-0x15D (GBATEK)
static uint16_t crc16(const uint8_t *data, size_t size) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

bool readRomInfo(const uint8_t *header, RomInfo &info) {
    // Use the stored header CRC, which tells versions and hacks apart; a wrong one (some badly rebuilt hacks) is only
    // reported, since the emulator and the DSi run those games anyway
    uint16_t stored = header[0x15E] | (header[0x15F] << 8);
    info.crcOk = (crc16(header, 0x15E) == stored);

    // Keep the game code readable, replacing anything that isn't a letter or digit
    std::string code;
    for (int i = 0; i < 4; i++) {
        char c = header[0x0C + i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        code += ok ? c : '_';
    }
    static const char digits[] = "0123456789ABCDEF";
    info.code = code;
    info.key = code + '-';
    for (int shift = 12; shift >= 0; shift -= 4)
        info.key += digits[(stored >> shift) & 0xF];

    // The title is 12 bytes, padded with zeros
    info.title.clear();
    for (int i = 0; i < 12 && header[i]; i++)
        info.title += (header[i] >= 0x20 && header[i] < 0x7F) ? (char)header[i] : '?';
    while (!info.title.empty() && info.title.back() == ' ') info.title.pop_back();
    if (info.title.empty()) info.title = code;
    return true;
}

size_t contentSize(const uint8_t *data, size_t size) {
    while (size > 0 && data[size - 1] == 0xFF) size--;
    return size;
}

uint64_t hashSave(const uint8_t *data, size_t size) {
    size = contentSize(data, size);

    // All zeros is as blank as all 0xFF
    size_t i = 0;
    while (i < size && data[i] == 0) i++;
    if (i == size) return 0;

    // Two 32-bit lanes, since the DSi's ARM9 has no fast 64-bit multiply; this only needs to tell saves apart, not
    // resist anyone making collisions on purpose
    uint32_t a = 0x811C9DC5 ^ (uint32_t)size, b = 0x9E3779B9 + (uint32_t)size;
    for (i = 0; i < size; i++) {
        a = (a ^ data[i]) * 0x01000193;
        b = (b + data[i]) * 0x85EBCA77;
        b ^= b >> 13;
    }
    a ^= a >> 16; a *= 0x7FEB352D; a ^= a >> 15;
    b ^= b >> 16; b *= 0x846CA68B; b ^= b >> 16;
    uint64_t hash = ((uint64_t)a << 32) | b;
    return hash ? hash : 1;
}

size_t targetSize(size_t content, size_t receiverSize, size_t senderSize) {
    return (receiverSize && content <= receiverSize) ? receiverSize : senderSize;
}

Action decide(uint64_t dsi, uint64_t other, bool hasBase, uint64_t base) {
    if (dsi == other) return Action::Same;
    if (dsi == 0) return Action::Download;
    if (other == 0) return Action::Upload;
    if (hasBase && dsi == base) return Action::Download;
    if (hasBase && other == base) return Action::Upload;
    return Action::Conflict;
}

std::string hex64(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; i--, value >>= 4)
        text[i] = digits[value & 0xF];
    return text;
}

uint64_t parseHex64(const char *text) {
    uint64_t value = 0;
    for (; *text; text++) {
        char c = *text;
        int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
            (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (digit < 0) break;
        value = (value << 4) | digit;
    }
    return value;
}

std::vector<int> pairGames(const std::vector<RomInfo> &dsi, const std::vector<RomInfo> &other) {
    std::map<std::string, int> byKey, codeCount, otherByCode, otherCodeCount;
    for (int i = (int)other.size() - 1; i >= 0; i--) {
        byKey[other[i].key] = i; // The first of any duplicates wins
        otherByCode[other[i].code] = i;
        otherCodeCount[other[i].code]++;
    }
    for (const RomInfo &rom : dsi) codeCount[rom.code]++;

    std::vector<int> pairs(dsi.size(), -1);
    std::vector<bool> taken(other.size(), false);
    for (size_t i = 0; i < dsi.size(); i++) {
        auto it = byKey.find(dsi[i].key);
        if (it != byKey.end() && !taken[it->second]) {
            pairs[i] = it->second;
            taken[it->second] = true;
        }
    }
    for (size_t i = 0; i < dsi.size(); i++) {
        if (pairs[i] >= 0 || codeCount[dsi[i].code] != 1 || otherCodeCount[dsi[i].code] != 1) continue;
        // Homebrew shares placeholder codes, so only real ones pair this way
        const std::string &code = dsi[i].code;
        if (code == "____" || code == "####" || code == "HNAA" || code == "0000") continue;
        int j = otherByCode[code];
        if (!taken[j]) {
            pairs[i] = j;
            taken[j] = true;
        }
    }
    return pairs;
}

bool readFile(const std::string &path, std::vector<uint8_t> &data) {
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bool ok = size >= 0 && (size_t)size <= MAX_SAVE;
    if (ok) {
        data.resize(size);
        ok = fread(data.data(), 1, size, file) == (size_t)size;
    }
    fclose(file);
    return ok;
}

bool replaceFile(const std::string &path, const uint8_t *data, size_t size) {
    std::string temp = path + ".tmp", backup = path + ".bak";
    FILE *file = fopen(temp.c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    ok = (fclose(file) == 0) && ok;
    if (!ok) {
        remove(temp.c_str());
        return false;
    }

    // Keep the save being replaced; with no save there yet, there's nothing to keep
    FILE *old = fopen(path.c_str(), "rb");
    if (old) {
        fclose(old);
        remove(backup.c_str());
        if (rename(path.c_str(), backup.c_str()) != 0) {
            remove(temp.c_str());
            return false;
        }
    }
    return rename(temp.c_str(), path.c_str()) == 0;
}

} // namespace SaveSync
