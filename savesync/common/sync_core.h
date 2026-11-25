/*
    Copyright 2026 michyprima

    Save sync logic shared by Maelle and its DSi app: naming games, hashing saves and deciding which way each save goes.
    Plain C++ with no exceptions, so the DSi build can use it as is.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SaveSync {

constexpr int PORT = 5821;
constexpr int PROTOCOL = 1;
constexpr size_t MAX_SAVE = 16 * 1024 * 1024;
constexpr size_t HEADER_SIZE = 0x160;

struct RomInfo {
    std::string key;   // Game code and header CRC, such as "IRBO-1A2B"
    std::string code;  // Game code alone, to pair versions of a game whose headers differ
    std::string title; // Title from the header, for showing
    bool crcOk;        // Whether the header CRC matches the header
};

// Read a ROM's identity from its first HEADER_SIZE bytes
bool readRomInfo(const uint8_t *header, RomInfo &info);

// A save's content size: its size without the 0xFF bytes at its end, which are erased memory that a save chip of any
// size reads the same, so the same save padded to two sizes counts as one
size_t contentSize(const uint8_t *data, size_t size);

// Hash a save's content; 0 means there's no save, or it's blank (all 0x00 or all 0xFF, as a fresh save file is)
uint64_t hashSave(const uint8_t *data, size_t size);

// The size to write a received save at: the receiver's own size if the content fits it, so the save chip size the
// receiver already uses stays, otherwise the sender's
size_t targetSize(size_t content, size_t receiverSize, size_t senderSize);

// Which way a save goes, seen from the DSi app
enum class Action { Same, Upload, Download, Conflict, Skip };

// Decide from the DSi's hash, the other side's hash and, if there's been a sync before, the hash both had then.
// A blank or missing save never overwrites one with data, and a save only one side changed goes to the other
Action decide(uint64_t dsi, uint64_t other, bool hasBase, uint64_t base);

// Pair the DSi's games with the other side's: the same key first, then the same game code when it's the only one
// with that code on both sides, which pairs versions of a game. Returns, for each DSi game, the other side's index or -1
std::vector<int> pairGames(const std::vector<RomInfo> &dsi, const std::vector<RomInfo> &other);

// Read a whole file, false if it can't be opened or is too big
bool readFile(const std::string &path, std::vector<uint8_t> &data);

// Replace a file safely: write a temporary file, keep the old one as <path>.bak, then rename the new one into place
bool replaceFile(const std::string &path, const uint8_t *data, size_t size);

std::string hex64(uint64_t value);
uint64_t parseHex64(const char *text);

} // namespace SaveSync
