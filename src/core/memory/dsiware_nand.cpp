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

#include <cstring>
#include "../core.h"
#include "../file_lock.h"

// Little-endian fields, whatever the host's byte order
static void put16(uint8_t *data, uint16_t value) {
    data[0] = value;
    data[1] = value >> 8;
}

static void put32(uint8_t *data, uint32_t value) {
    put16(data, value);
    put16(data + 2, value >> 16);
}

static uint32_t get32(const uint8_t *data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (uint32_t(data[3]) << 24);
}

DsiWareNand::~DsiWareNand() {
    flush();
}

void DsiWareNand::sha1(const uint8_t *data, uint32_t size, uint8_t *hash) {
    // Hash a small buffer with SHA-1, for the NAND's counter
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::vector<uint8_t> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back(uint64_t(size) * 8 >> (i * 8));
    for (size_t block = 0; block < msg.size(); block += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = &msg[block + i * 4];
            w[i] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 20; i++)
        hash[i] = h[i / 4] >> (24 - (i % 4) * 8);
}

std::vector<uint8_t> DsiWareNand::newSave(uint32_t size) {
    // Format a new save as the DSi does when a title is installed: a FAT12 volume filling it, with no files
    // The geometry fits as many sectors as possible as tracks times heads squared, after NTM's sav.c (via melonDS)
    std::vector<uint8_t> data(size, 0);
    if (size < 0x200) return data;
    uint32_t maxSectors = std::min(size >> 9, 0xFFFFU), tracks = 1, heads = 1, sectors = 0, next = 0;
    while (next <= maxSectors) {
        next = tracks * (heads + 1) * (heads + 1);
        if (next <= maxSectors) {
            heads++;
            sectors = next;
            tracks++;
            next = tracks * heads * heads;
            if (next <= maxSectors) sectors = next;
        }
    }
    if ((tracks + 1) * heads * heads <= maxSectors) {
        tracks++;
        sectors = tracks * heads * heads;
    }
    uint32_t cluster = (sectors > (8 << 10)) ? 8 : (sectors > (1 << 10)) ? 4 : 1;
    uint32_t clusters = (sectors + cluster - 1) / cluster;
    uint32_t fatSectors = (((clusters + 1) / 2) * 3 + 0x1FF) >> 9;

    data[0x00] = 0xE9;
    memcpy(&data[0x03], "MSWIN4.1", 8);
    put16(&data[0x0B], 0x200); // Bytes per sector
    data[0x0D] = cluster;
    put16(&data[0x0E], 1); // Reserved sectors
    data[0x10] = 2; // FATs
    put16(&data[0x11], (size < 0x8C000) ? 0x20 : 0x200); // Root folder entries
    put16(&data[0x13], sectors);
    data[0x15] = 0xF8; // Media
    put16(&data[0x16], fatSectors);
    put16(&data[0x18], tracks);
    put16(&data[0x1A], heads);
    data[0x24] = 0x05; // Drive number
    data[0x26] = 0x29; // Extended boot signature
    put32(&data[0x27], 0x12345678); // Serial number
    memcpy(&data[0x2B], "VOLUMELABEL", 11);
    memcpy(&data[0x36], "FAT12   ", 8);
    put16(&data[0x1FE], 0xAA55);
    return data;
}

int DsiWareNand::addFile(std::string shortName, int parent, uint32_t size, bool folder) {
    File file;
    file.shortName = shortName;
    file.parent = parent;
    file.size = size;
    file.folder = folder;
    files.push_back(file);
    return files.size() - 1;
}

void DsiWareNand::putEntry(uint8_t *dir, const std::string &name, uint8_t attributes, uint32_t cluster, uint32_t size) {
    // Fill a folder entry with an 8.3 name; names in small letters are stored in capitals with the flags that show them
    // small again, as Windows NT does, and the entry is dated when the DSi came out
    memset(dir, 0, 0x20);
    memset(dir, ' ', 11);
    if (name[0] == '.') {
        memcpy(dir, name.c_str(), name.size());
    }
    else {
        size_t dot = std::min(name.find('.'), name.size());
        bool lower[2] = {};
        for (size_t i = 0; i < name.size(); i++) {
            if (i == dot) continue;
            bool ext = (i > dot);
            size_t pos = ext ? 8 + (i - dot - 1) : i;
            if (pos >= (ext ? 11U : 8U)) continue;
            dir[pos] = toupper((unsigned char)name[i]);
            if (islower((unsigned char)name[i])) lower[ext] = true;
        }
        dir[0x0C] = (lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0);
    }
    dir[0x0B] = attributes;
    put16(&dir[0x10], ((2008 - 1980) << 9) | (11 << 5) | 1); // Created date
    put16(&dir[0x12], ((2008 - 1980) << 9) | (11 << 5) | 1); // Accessed date
    put16(&dir[0x18], ((2008 - 1980) << 9) | (11 << 5) | 1); // Modified date
    put16(&dir[0x1A], cluster);
    put32(&dir[0x1C], size);
}

void DsiWareNand::crypt(uint32_t sector, uint8_t *data) {
    // Encrypt or decrypt a sector with the NAND's key, counting 16-byte blocks from the start of the NAND
    uint32_t words[0x80];
    for (int i = 0; i < 0x80; i++)
        words[i] = get32(&data[i * 4]);
    core->aes.decryptCtr(keyX, keyY, iv, sector << 5, words, 0x80);
    for (int i = 0; i < 0x80; i++)
        put32(&data[i * 4], words[i]);
}

bool DsiWareNand::readPlain(uint32_t sector, uint8_t *data) {
    // Read a NAND sector decrypted, as edited here if it was
    auto it = changed.find(sector);
    if (it != changed.end()) {
        memcpy(data, it->second.data(), 0x200);
        return true;
    }
    if (!core->sdMmc.readNand(uint64_t(sector) << 9, data, 0x200)) return false;
    crypt(sector, data);
    return true;
}

std::vector<uint8_t> &DsiWareNand::edit(uint32_t sector) {
    // Get a NAND sector to change, keeping it here from then on
    auto it = changed.find(sector);
    if (it != changed.end()) return it->second;
    std::vector<uint8_t> data(0x200);
    readPlain(sector, data.data());
    return changed[sector] = data;
}

uint16_t DsiWareNand::fatEntry(uint32_t cluster) {
    uint8_t data[0x200];
    readPlain(fatStart + (cluster >> 8), data);
    return data[(cluster & 0xFF) * 2] | (data[(cluster & 0xFF) * 2 + 1] << 8);
}

void DsiWareNand::setFatEntry(uint32_t cluster, uint16_t value) {
    // Change an entry in every copy of the FAT
    for (uint32_t i = 0; i < fats; i++)
        put16(&edit(fatStart + i * fatSectors + (cluster >> 8))[(cluster & 0xFF) * 2], value);
}

std::vector<uint32_t> DsiWareNand::chain(uint32_t cluster) {
    // List a folder's sectors from its first cluster
    std::vector<uint32_t> sectors;
    for (uint32_t i = 0; cluster >= 2 && cluster < clusterCount + 2 && i < clusterCount; i++) {
        for (uint32_t j = 0; j < clusterSectors; j++)
            sectors.push_back(sectorOf(cluster) + j);
        cluster = fatEntry(cluster);
    }
    return sectors;
}

int DsiWareNand::findEntry(const std::vector<uint32_t> &sectors, const std::string &name, uint32_t &sector,
        uint32_t &offset) {
    // Find a folder entry by name and return its first cluster, or with an empty name find a free entry; -1 if neither
    uint8_t want[0x20];
    if (!name.empty()) putEntry(want, name, 0, 0, 0);
    for (uint32_t s : sectors) {
        uint8_t data[0x200];
        if (!readPlain(s, data)) return -1;
        for (uint32_t i = 0; i < 0x200; i += 0x20) {
            uint8_t *dir = &data[i];
            if (name.empty()) {
                if (dir[0] != 0x00 && dir[0] != 0xE5) continue;
            }
            else {
                if (dir[0] == 0x00) return -1;
                if (dir[0] == 0xE5 || dir[0x0B] == 0x0F || (dir[0x0B] & 0x08) || memcmp(dir, want, 11)) continue;
            }
            sector = s;
            offset = i;
            return dir[0x1A] | (dir[0x1B] << 8);
        }
    }
    return -1;
}

bool DsiWareNand::create(const uint8_t *header, std::string savePath) {
    files.clear();
    changed.clear();
    owned.clear();

    // Use the key and counter the DSi sets up for its NAND: AES slot 3 from the console ID, and the counter from a SHA-1
    // of the eMMC's CID, both from the NAND dump's footer
    uint32_t id[2] = { core->sdMmc.readConsoleId(0), core->sdMmc.readConsoleId(1) };
    uint32_t x[4] = { id[0], id[0] ^ 0x24EE6906, id[1] ^ 0xE65B601D, id[1] };
    uint32_t y[4] = { 0x0AB9DC76, 0xBD4DC4D3, 0x202DDD1D, 0xE1A00005 };
    memcpy(keyX, x, sizeof(keyX));
    memcpy(keyY, y, sizeof(keyY));
    uint8_t cid[16], hash[20];
    for (int i = 0; i < 4; i++)
        put32(&cid[i * 4], core->sdMmc.readCid(i));
    sha1(cid, sizeof(cid), hash);
    for (int i = 0; i < 4; i++)
        iv[i] = get32(&hash[i * 4]);

    // Find the main partition and its FAT16 layout
    uint8_t mbr[0x200], boot[0x200];
    if (!readPlain(0, mbr) || mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) {
        LOG_CRIT("DSiWare can't be installed: the NAND's partition table can't be read\n");
        return false;
    }
    uint32_t partStart = get32(&mbr[0x1BE + 8]);
    if (!readPlain(partStart, boot) || boot[0x1FE] != 0x55 || boot[0x1FF] != 0xAA || boot[0x0B] || boot[0x0C] != 2) {
        LOG_CRIT("DSiWare can't be installed: the NAND's main partition can't be read\n");
        return false;
    }
    clusterSectors = boot[0x0D];
    fats = boot[0x10];
    fatSectors = boot[0x16] | (boot[0x17] << 8);
    fatStart = partStart + (boot[0x0E] | (boot[0x0F] << 8));
    uint32_t rootStart = fatStart + fats * fatSectors;
    uint32_t rootSectors = (((boot[0x11] | (boot[0x12] << 8)) * 0x20) + 0x1FF) >> 9;
    uint32_t total = (boot[0x13] | (boot[0x14] << 8)) ? (boot[0x13] | (boot[0x14] << 8)) : get32(&boot[0x20]);
    dataStart = rootStart + rootSectors;
    clusterCount = std::min((total - (dataStart - partStart)) / clusterSectors, fatSectors * 0x100 - 2);
    uint32_t clusterSize = clusterSectors << 9;

    // Find where the title's folder goes, in title/<high>; a title already installed is hidden by the new folder
    char ids[2][9];
    snprintf(ids[0], sizeof(ids[0]), "%08x", U8TO32(header, 0x234));
    snprintf(ids[1], sizeof(ids[1]), "%08x", U8TO32(header, 0x230));
    folder = std::string("title/") + ids[0] + "/" + ids[1];
    std::vector<uint32_t> root;
    for (uint32_t i = 0; i < rootSectors; i++)
        root.push_back(rootStart + i);
    uint32_t sector, offset;
    int title = findEntry(root, "title", sector, offset);
    if (title < 2) {
        LOG_CRIT("DSiWare can't be installed: the NAND has no title folder\n");
        return false;
    }
    int high = findEntry(chain(title), ids[0], sector, offset);
    uint32_t topParent = (high >= 2) ? high : title;
    if ((high >= 2 && findEntry(chain(high), ids[1], sector, offset) < 0 && findEntry(chain(high), "", sector, offset) < 0) ||
        (high < 2 && findEntry(chain(title), "", sector, offset) < 0)) {
        LOG_CRIT("DSiWare can't be installed: no room in the NAND's title folder\n");
        return false;
    }
    int low = (high >= 2) ? addFile(ids[1], -1, 0, true) : addFile(ids[1], addFile(ids[0], -1, 0, true), 0, true);
    int content = addFile("content", low, 0, true);
    int data = addFile("data", low, 0, true);
    addFile("00000000.app", content, uint32_t(core->cartridgeNds.getRomSize()));

    // Add the saves the header asks for, from the files next to the ROM or made new: the DSi menu formats public and
    // private saves and leaves banner.sav blank (as melonDS's title import does); a save of the wrong size keeps what fits
    uint32_t sizes[3] = { U8TO32(header, 0x238), U8TO32(header, 0x23C), (header[0x1BF] & BIT(2)) ? 0x4000U : 0 };
    const char *exts[3] = { ".pub", ".prv", ".bnr" };
    std::string base = savePath.substr(0, savePath.rfind('.'));
    for (int i = 0; i < 3; i++) {
        saves[i] = (sizes[i] != 0);
        if (!saves[i]) continue;
        File &file = files[addFile(saveNames[i], data, sizes[i])];
        file.path = base + exts[i];
        file.data = (i < 2) ? newSave(sizes[i]) : std::vector<uint8_t>(sizes[i], 0);
        std::lock_guard<FileLock> guard(fileLock());
        if (FILE *in = fopen(file.path.c_str(), "rb")) {
            fread(file.data.data(), 1, sizes[i], in);
            fclose(in);
        }
        else {
            file.dirty = true;
        }
    }

    // Give each folder and file free clusters, chained in the FAT; folders get one cluster for their entries
    uint32_t needed = 0;
    for (File &file : files)
        needed += file.folder ? 1 : (file.size + clusterSize - 1) / clusterSize;
    std::vector<uint32_t> free;
    for (uint32_t s = 0; s < fatSectors && free.size() < needed; s++) {
        uint8_t fat[0x200];
        readPlain(fatStart + s, fat);
        for (uint32_t i = 0; i < 0x100 && free.size() < needed; i++) {
            uint32_t c = (s << 8) + i;
            if (c >= 2 && c < clusterCount + 2 && !fat[i * 2] && !fat[i * 2 + 1]) free.push_back(c);
        }
    }
    if (free.size() < needed) {
        LOG_CRIT("DSiWare can't be installed: the NAND needs %u free clusters, but has %u\n", needed,
            uint32_t(free.size()));
        files.clear();
        changed.clear();
        return false;
    }
    uint32_t next = 0;
    for (size_t i = 0; i < files.size(); i++) {
        File &file = files[i];
        if (file.folder) {
            file.size = clusterSize;
            file.data.assign(clusterSize, 0);
        }
        uint32_t count = (file.size + clusterSize - 1) / clusterSize;
        for (uint32_t j = 0; j < count; j++) {
            uint32_t c = free[next++];
            file.clusters.push_back(c);
            owned[c] = { int(i), j * clusterSize };
            setFatEntry(c, (j + 1 < count) ? free[next] : 0xFFFF);
        }
    }

    // Fill the new folders' entries, and put the top new folder in its folder on the NAND
    std::vector<int> used(files.size(), 2);
    for (size_t i = 0; i < files.size(); i++) {
        File &file = files[i];
        uint32_t first = file.clusters.empty() ? 0 : file.clusters[0];
        if (file.folder) {
            uint32_t up = (file.parent < 0) ? topParent : files[file.parent].clusters[0];
            putEntry(&file.data[0x00], ".", 0x10, first, 0);
            putEntry(&file.data[0x20], "..", 0x10, up, 0);
        }
        uint8_t *dir = (file.parent < 0) ? &edit(sector)[offset] : &files[file.parent].data[used[file.parent]++ * 0x20];
        putEntry(dir, file.shortName, file.folder ? 0x10 : 0x20, first, file.folder ? 0 : file.size);
    }
    LOG_INFO("DSiWare installed in memory as nand:/%s, %u clusters\n", folder.c_str(), needed);
    return true;
}

bool DsiWareNand::owns(uint64_t offset) {
    // Check if a NAND sector is one changed here or in a new file's cluster
    uint32_t sector = offset >> 9;
    if (changed.count(sector)) return true;
    return sector >= dataStart && owned.count((sector - dataStart) / clusterSectors + 2);
}

void DsiWareNand::read(uint64_t offset, uint8_t *data, uint32_t size) {
    // Read part of a sector served from here, encrypted as the NAND holds it
    uint32_t sector = offset >> 9, start = offset & 0x1FF;
    uint8_t plain[0x200] = {};
    auto it = changed.find(sector);
    if (it != changed.end()) {
        memcpy(plain, it->second.data(), 0x200);
    }
    else {
        uint32_t cluster = (sector - dataStart) / clusterSectors + 2;
        auto &own = owned[cluster];
        File &file = files[own.first];
        uint32_t pos = own.second + ((sector - sectorOf(cluster)) << 9);
        uint32_t count = std::min(0x200U, file.size - std::min(file.size, pos));
        if (file.path.empty() && !file.folder)
            core->cartridgeNds.readRom(pos, plain, count);
        else
            memcpy(plain, &file.data[pos], count);
        memset(&plain[count], 0, 0x200 - count);
    }
    crypt(sector, plain);
    memcpy(data, &plain[start], std::min(size, 0x200 - start));
}

void DsiWareNand::write(uint64_t offset, const uint8_t *data, uint32_t size) {
    // Write part of a sector served from here, decrypted; the title's own file can't change
    uint32_t sector = offset >> 9, start = offset & 0x1FF;
    uint8_t sec[0x200];
    read(sector << 9, sec, 0x200);
    memcpy(&sec[start], data, std::min(size, 0x200 - start));
    crypt(sector, sec);
    auto it = changed.find(sector);
    if (it != changed.end()) {
        memcpy(it->second.data(), sec, 0x200);
        return;
    }
    uint32_t cluster = (sector - dataStart) / clusterSectors + 2;
    auto &own = owned[cluster];
    File &file = files[own.first];
    if (file.path.empty() && !file.folder) return;
    uint32_t pos = own.second + ((sector - sectorOf(cluster)) << 9);
    uint32_t count = std::min(0x200U, file.size - std::min(file.size, pos));
    if (count && memcmp(&file.data[pos], sec, count)) {
        memcpy(&file.data[pos], sec, count);
        file.dirty = !file.path.empty();
    }
}

void DsiWareNand::flush() {
    // Write the saves that changed back to their files, whole, since they're small
    std::lock_guard<FileLock> guard(fileLock());
    for (File &file : files) {
        if (!file.dirty || file.path.empty()) continue;
        if (FILE *out = fopen(file.path.c_str(), "wb")) {
            fwrite(file.data.data(), 1, file.size, out);
            fclose(out);
            file.dirty = false;
        }
    }
}
