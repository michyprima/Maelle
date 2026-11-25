/*
    Copyright 2026 michyprima

    Checks save sync's shared rules: hashing, sizes and which way each save goes.
*/

#include <cstdio>
#include <cstring>
#include <vector>
#include "../common/sync_core.h"

using namespace SaveSync;

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main() {
    // Blank saves hash to 0, whatever their size or fill
    std::vector<uint8_t> ff(512 * 1024, 0xFF), zero(8 * 1024, 0);
    CHECK(hashSave(ff.data(), ff.size()) == 0);
    CHECK(hashSave(zero.data(), zero.size()) == 0);
    CHECK(hashSave(nullptr, 0) == 0);

    // The same save padded with 0xFF to a bigger chip is the same save
    std::vector<uint8_t> small(64 * 1024, 0xFF);
    for (size_t i = 0; i < 1000; i++) small[i * 37] = (uint8_t)(i * 13 + 1);
    std::vector<uint8_t> big = small;
    big.resize(512 * 1024, 0xFF);
    uint64_t h = hashSave(small.data(), small.size());
    CHECK(h != 0);
    CHECK(hashSave(big.data(), big.size()) == h);

    // But padding with zeros, or any byte changing, is a different save
    std::vector<uint8_t> zeroPad = small;
    zeroPad.resize(128 * 1024, 0);
    CHECK(hashSave(zeroPad.data(), zeroPad.size()) != h);
    for (size_t pos : {0, 1, 500, 36962}) {
        std::vector<uint8_t> changed = small;
        changed[pos] ^= 1;
        CHECK(hashSave(changed.data(), changed.size()) != h);
    }
    // Swapping two bytes changes it too
    std::vector<uint8_t> swapped = small;
    std::swap(swapped[0], swapped[37]);
    CHECK(hashSave(swapped.data(), swapped.size()) != h);

    // Content size and the size to write at
    CHECK(contentSize(big.data(), big.size()) == contentSize(small.data(), small.size()));
    CHECK(contentSize(ff.data(), ff.size()) == 0);
    CHECK(targetSize(1000, 512 * 1024, 64 * 1024) == 512 * 1024); // fits the receiver's chip, keep it
    CHECK(targetSize(100 * 1024, 64 * 1024, 512 * 1024) == 512 * 1024); // too big for it, take the sender's
    CHECK(targetSize(1000, 0, 64 * 1024) == 64 * 1024); // receiver has none

    // Deciding, from the DSi's point of view (Upload = DSi to Maelle)
    const uint64_t A = 0xA, B = 0xB, C = 0xC;
    CHECK(decide(A, A, false, 0) == Action::Same);
    CHECK(decide(0, 0, false, 0) == Action::Same);
    CHECK(decide(0, A, false, 0) == Action::Download);
    CHECK(decide(A, 0, false, 0) == Action::Upload);
    CHECK(decide(A, B, false, 0) == Action::Conflict); // first sync, both have different saves
    CHECK(decide(A, B, true, A) == Action::Download);  // only Maelle's changed
    CHECK(decide(A, B, true, B) == Action::Upload);    // only the DSi's changed
    CHECK(decide(A, B, true, C) == Action::Conflict);  // both changed
    CHECK(decide(0, B, true, B) == Action::Download);  // DSi save gone or blank: never sends a blank over data
    CHECK(decide(A, 0, true, A) == Action::Upload);    // same the other way

    // ROM headers: game code, CRC and title
    uint8_t header[HEADER_SIZE] = {};
    memcpy(header, "POKEMON W2\0\0", 12);
    memcpy(header + 0x0C, "IRDO", 4);
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 0x15E; i++) {
        crc ^= header[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    header[0x15E] = crc & 0xFF;
    header[0x15F] = crc >> 8;
    RomInfo info;
    CHECK(readRomInfo(header, info));
    CHECK(info.crcOk);
    CHECK(info.code == "IRDO");
    CHECK(info.title == "POKEMON W2");
    char expect[16];
    snprintf(expect, sizeof(expect), "IRDO-%04X", crc);
    CHECK(info.key == expect);
    header[0x15E] ^= 1;
    CHECK(readRomInfo(header, info) && !info.crcOk);
    memcpy(header + 0x0C, "##\x01#", 4);
    CHECK(readRomInfo(header, info) && info.code == "____");

    // Pairing: exact key, then a unique game code, never placeholder codes or a game taken twice
    auto rom = [](const char *key) { RomInfo r; r.key = key; r.code = std::string(key, 4); r.crcOk = true; return r; };
    std::vector<RomInfo> dsi = { rom("IRBO-1111"), rom("IRBO-2222"), rom("AMCE-AAAA"), rom("IPKE-0001"),
        rom("####-1234"), rom("####-9999"), rom("YXXE-0001") };
    std::vector<RomInfo> other = { rom("AMCE-BBBB"), rom("IRBO-2222"), rom("IRBO-3333"), rom("IPKE-0002"),
        rom("####-5678"), rom("####-9999"), rom("ZZZE-0001") };
    std::vector<int> pairs = pairGames(dsi, other);
    CHECK(pairs[0] == -1); // IRBO-1111: two IRBO on each side, no exact match
    CHECK(pairs[1] == 1);  // exact
    CHECK(pairs[2] == 0);  // AMCE: one version on each side
    CHECK(pairs[3] == 3);  // IPKE likewise
    CHECK(pairs[4] == -1); // placeholder code never pairs by code
    CHECK(pairs[5] == 5);  // but exact keys do
    CHECK(pairs[6] == -1); // not on the other side

    CHECK(hex64(0x0123456789abcdefULL) == "0123456789abcdef");
    CHECK(parseHex64("0123456789ABCDEF") == 0x0123456789abcdefULL);

    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
