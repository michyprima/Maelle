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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Core;

// A DSiWare title booted directly, installed on the NAND only in memory: its folder, title/<high>/<low> with content/ and
// data/, is added to the NAND's main partition in free clusters, as the DSi Shop would put it there
// Only the sectors this changes are served from here: the FAT and folder sectors it edits, and the clusters of the new
// folders and files, whose data comes from the ROM and the save files next to it; every other NAND sector is read and
// written as before, so system files like the shared font stay reachable, and nothing here ever reaches the NAND file
// Titles need their files where the DSi menu puts them: the SDK opens banner.sav (Brain Age Express) as nand:/<banner>,
// always on the NAND, and without it those titles stop with "Download failed"
class DsiWareNand {
public:
    DsiWareNand(Core *core): core(core) {}
    ~DsiWareNand();

    bool active() { return !files.empty(); }
    bool create(const uint8_t *header, std::string savePath);

    // NAND accesses, by byte offset; owns() says whether a sector is served from here
    bool owns(uint64_t offset);
    void read(uint64_t offset, uint8_t *data, uint32_t size);
    void write(uint64_t offset, const uint8_t *data, uint32_t size);
    void flush();

    // Paths to the title's file and its saves, for the device list; a save the title doesn't have is empty
    std::string appPath() { return "nand:/" + folder + "/content/00000000.app"; }
    std::string savePath(int i) { return saves[i] ? "nand:/" + folder + "/data/" + saveNames[i] : ""; }

private:
    Core *core;

    // Folders are files too, with their entries as data; the first file is the title's own, read from the ROM
    struct File {
        std::string shortName, path; // Path is the host file for saves
        uint32_t size = 0;
        int parent = -1; // Folder holding it, or -1 for the title folder's parent on the NAND
        bool folder = false, dirty = false;
        std::vector<uint8_t> data;
        std::vector<uint32_t> clusters;
    };

    static constexpr const char *saveNames[3] = { "public.sav", "private.sav", "banner.sav" };

    std::vector<File> files;
    std::string folder; // The title's folder, like title/00030004/4b4e3945
    bool saves[3] = {};

    std::unordered_map<uint32_t, std::vector<uint8_t>> changed; // NAND sectors edited here, decrypted
    std::unordered_map<uint32_t, std::pair<int, uint32_t>> owned; // Clusters of the new files: file and byte offset
    uint32_t keyX[4] = {}, keyY[4] = {}, iv[4] = {};
    uint32_t fatStart = 0, fatSectors = 0, fats = 0, dataStart = 0, clusterSectors = 0, clusterCount = 0;

    void crypt(uint32_t sector, uint8_t *data);
    bool readPlain(uint32_t sector, uint8_t *data);
    std::vector<uint8_t> &edit(uint32_t sector);
    uint16_t fatEntry(uint32_t cluster);
    void setFatEntry(uint32_t cluster, uint16_t value);
    std::vector<uint32_t> chain(uint32_t cluster);
    int findEntry(const std::vector<uint32_t> &sectors, const std::string &name, uint32_t &sector, uint32_t &offset);
    uint32_t sectorOf(uint32_t cluster) { return dataStart + (cluster - 2) * clusterSectors; }
    int addFile(std::string shortName, int parent, uint32_t size = 0, bool folder = false);

    static std::vector<uint8_t> newSave(uint32_t size);
    static void putEntry(uint8_t *dir, const std::string &name, uint8_t attributes, uint32_t cluster, uint32_t size);
    static void sha1(const uint8_t *data, uint32_t size, uint8_t *hash);
};
