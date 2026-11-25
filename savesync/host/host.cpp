/*
    Copyright 2026 michyprima

    Stand-in for Maelle's side of save sync, for testing without a Wii U: serves the saves of a folder of ROMs, found
    the way Maelle finds them (next to the ROM, or in a saves folder like the "saves folder" setting).

    Usage: maelle-sync-host <ROM folder> [--saves <folder>] [--name <name>] [--busy]
*/

#include <dirent.h>
#include <sys/stat.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../common/sync_server.h"

using namespace SaveSync;

static std::string romDir, savesDir;

static bool hasExtension(const std::string &name, const char *ext) {
    size_t n = strlen(ext);
    if (name.size() < n) return false;
    return strcasecmp(name.c_str() + name.size() - n, ext) == 0;
}

static void scan(const std::string &dir, std::vector<ServerSave> &out, int depth) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (dirent *e = readdir(d)) {
        std::string name = e->d_name;
        if (name[0] == '.') continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) scan(path, out, depth + 1);
            continue;
        }
        if (!hasExtension(name, ".nds") && !hasExtension(name, ".dsi")) continue;

        uint8_t header[HEADER_SIZE];
        FILE *file = fopen(path.c_str(), "rb");
        if (!file) continue;
        bool ok = fread(header, 1, HEADER_SIZE, file) == HEADER_SIZE;
        fclose(file);
        ServerSave save;
        if (!ok || !readRomInfo(header, save.rom)) continue;

        // As Cartridge::setRom names the save
        std::string base = name.substr(0, name.rfind('.'));
        save.savePath = savesDir.empty() ? (dir + "/" + base + ".sav") : (savesDir + "/" + base + ".sav");
        out.push_back(save);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ROM folder> [--saves <folder>] [--name <name>] [--busy]\n", argv[0]);
        return 1;
    }
    char real[PATH_MAX];
    romDir = realpath(argv[1], real) ? real : argv[1];
    std::string name = "Mac stand-in";
    bool busy = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--saves") && i + 1 < argc) savesDir = realpath(argv[++i], real) ? real : argv[i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--busy")) busy = true;
    }

    Server server;
    server.name = name;
    // A stable ID for this collection of saves, so the DSi keeps a separate sync history for it
    std::string where = savesDir.empty() ? romDir : savesDir;
    server.id = hex64(hashSave((const uint8_t*)where.data(), where.size()));
    server.listSaves = [] {
        std::vector<ServerSave> list;
        scan(romDir, list, 0);
        return list;
    };
    server.busy = [busy] { return busy; };
    server.log = [](const std::string &text) { printf("%s\n", text.c_str()); fflush(stdout); };

    if (!server.start()) {
        fprintf(stderr, "couldn't open port %d\n", PORT);
        return 1;
    }
    while (true) server.poll(1000);
}
