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

// The save sync screen: while it's open, Maelle serves its DS saves to the Maelle Sync app on a real DSi, which does
// the syncing. The server is in savesync/common, shared with the Mac stand-in

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <map>
#include <mutex>
#include <strings.h>
#include <sys/stat.h>
#include <thread>

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nn/ac/ac_c.h>

#include "console_ui.h"
#include "sync_server.h"
#include "../../core/file_lock.h"
#include "../../core/settings.h"
#include "../../core/worker_thread.h"

#define SCALEH(x, h) (((x) * (h)) / 720)
#define SCALE(x) SCALEH(x, menuHeight)

std::string ConsoleUI::syncId;

namespace {

enum SyncState { SYNC_CONNECTING, SYNC_READY, SYNC_NO_NETWORK, SYNC_NO_PORT };

// Shared between the screen and the server's thread
std::mutex syncMutex;
std::deque<std::string> syncLog;
std::string syncAddress;
std::atomic<int> syncState(SYNC_CONNECTING);
std::atomic<bool> syncQuit(false), syncRunning(false);

void addLog(const std::string &text) {
    std::lock_guard<std::mutex> guard(syncMutex);
    syncLog.push_back(text.empty() ? text : char(toupper(text[0])) + text.substr(1));
    while (syncLog.size() > 9) syncLog.pop_front();
}

bool isNds(const std::string &name) {
    return name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".nds") == 0;
}

// What's known about each ROM file while the screen is open, so later lists don't read their headers again
struct CachedRom {
    bool valid;
    SaveSync::RomInfo info;
};

void scanRoms(const std::string &dir, int depth, std::map<std::string, CachedRom> &cache,
        std::vector<SaveSync::ServerSave> &out) {
    // List the folder in one go, so the lock is only held for it
    std::vector<std::pair<std::string, bool>> entries;
    {
        std::lock_guard<FileLock> guard(fileLock());
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        while (dirent *e = readdir(d)) {
            std::string name = e->d_name;
            if (name.empty() || name[0] == '.') continue;
            bool folder;
            if (e->d_type == DT_DIR || e->d_type == DT_REG) {
                folder = (e->d_type == DT_DIR);
            }
            else {
                struct stat st;
                if (stat((dir + "/" + name).c_str(), &st) != 0) continue;
                folder = S_ISDIR(st.st_mode);
            }
            if (folder || isNds(name)) entries.emplace_back(name, folder);
        }
        closedir(d);
    }

    for (auto &entry : entries) {
        const std::string &name = entry.first;
        std::string path = dir + "/" + name;
        if (entry.second) {
            // The Wii U's own folder holds apps and system files, not ROMs
            if (depth < 5 && !(depth == 0 && strcasecmp(name.c_str(), "wiiu") == 0))
                scanRoms(path, depth + 1, cache, out);
            continue;
        }

        auto it = cache.find(path);
        if (it == cache.end()) {
            CachedRom rom = { false, {} };
            uint8_t header[SaveSync::HEADER_SIZE];
            {
                std::lock_guard<FileLock> guard(fileLock());
                if (FILE *file = fopen(path.c_str(), "rb")) {
                    rom.valid = fread(header, 1, sizeof(header), file) == sizeof(header);
                    fclose(file);
                }
            }
            rom.valid = rom.valid && SaveSync::readRomInfo(header, rom.info);
            it = cache.emplace(path, rom).first;
        }
        if (!it->second.valid) continue;

        // Where Cartridge::setRom keeps the save
        SaveSync::ServerSave save;
        save.rom = it->second.info;
        std::string base = name.substr(0, name.rfind('.'));
        save.savePath = Settings::savesFolder ? (Settings::basePath + "/saves/" + base + ".sav") :
            (dir + "/" + base + ".sav");
        out.push_back(save);
    }
}

void runServer(std::string root, std::string id) {
    OSSetThreadName(OSGetCurrentThread(), "Maelle Sync");

    // Connect the console to its network if it isn't already, closing only a connection made here when done
    ACInitialize();
    BOOL connected = FALSE;
    bool opened = false;
    if (NNResult_IsFailure(ACIsApplicationConnected(&connected)) || !connected)
        opened = NNResult_IsSuccess(ACConnect());
    uint32_t ip = 0;
    if (syncQuit.load()) {
        // Left before the network was up
    }
    else if (NNResult_IsFailure(ACGetAssignedAddress(&ip)) || !ip) {
        syncState.store(SYNC_NO_NETWORK);
    }
    else {
        char address[16];
        snprintf(address, sizeof(address), "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFF),
            (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF));
        {
            std::lock_guard<std::mutex> guard(syncMutex);
            syncAddress = address;
        }

        std::map<std::string, CachedRom> cache;
        SaveSync::Server server;
        server.id = id;
        server.name = "Maelle on Wii U";
        server.listSaves = [&] {
            std::vector<SaveSync::ServerSave> list;
            scanRoms(root, 0, cache, list);
            return list;
        };
        // The screen only opens with no game running, so saves are never in use
        server.busy = [] { return false; };
        server.log = addLog;
        server.lockFiles = [] { fileLock().lock(); };
        server.unlockFiles = [] { fileLock().unlock(); };

        if (server.start()) {
            syncState.store(SYNC_READY);
            server.stopRequested = [] { return syncQuit.load(); };
            while (!syncQuit.load()) server.poll(100);
            server.stop();
        }
        else {
            syncState.store(SYNC_NO_PORT);
        }
    }

    if (opened) ACClose();
    ACFinalize();
}

} // namespace

void ConsoleUI::stopSync() {
    // Stop the server and wait for its thread to release the network, for exits from the HOME menu while it runs
    syncQuit.store(true);
    for (int i = 0; i < 1000 && syncRunning.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void ConsoleUI::syncMenu() {
    // Saves can't be swapped under a game that's open, so close it first, which also writes its save
    if (core) {
        if (!message("Sync Saves with DSi", "The game has to be closed to sync saves.\n"
                "Any unsaved progress will be lost. Close it?", 1))
            return;
        closeCore();
    }

    // Give this Wii U an ID once, so the DSi keeps its sync history apart from other places it syncs with
    if (syncId.empty()) {
        uint64_t seed = OSGetTime() ^ ((uint64_t)OSGetSystemTime() << 32);
        syncId = SaveSync::hex64(SaveSync::hashSave((const uint8_t*)&seed, sizeof(seed)) | 1);
        Settings::save();
    }

    // Start the server on core 2, at the priority network threads need there
    {
        std::lock_guard<std::mutex> guard(syncMutex);
        syncLog.clear();
        syncAddress.clear();
    }
    syncState.store(SYNC_CONNECTING);
    syncQuit.store(false);
    syncRunning.store(true);
    std::string root = basePath, id = syncId;
    WorkerThread *thread = new WorkerThread(WorkerThread::CORE_2, [root, id] {
        runServer(root, id);
        syncRunning.store(false);
    });

    std::string actionB = "\x81 Back";
    int boundsB = 1218 - stringWidth(actionB) * 34 / 48;
    bool touchStarted = false;
    MenuTouch touchStart(false, 0, 0);

    while (true) {
        startFrame(palette[0]);
        drawString("Sync Saves with DSi", SCALE(72), SCALE(30), SCALE(42), palette[1]);
        drawRectangle(SCALE(30), SCALE(88), SCALE(1220), lineHeight, palette[1]);
        drawRectangle(SCALE(30), SCALE(648), SCALE(1220), lineHeight, palette[1]);
        drawString(actionB, SCALE(1218), SCALE(667), SCALE(34), palette[1], true);

        std::vector<std::string> log;
        std::string address;
        {
            std::lock_guard<std::mutex> guard(syncMutex);
            log.assign(syncLog.begin(), syncLog.end());
            address = syncAddress;
        }

        switch (syncState.load()) {
        case SYNC_CONNECTING:
            drawString("Connecting to the network...", SCALE(90), SCALE(124), SCALE(38), palette[1]);
            break;
        case SYNC_NO_NETWORK:
            drawString("Couldn't connect to the network.", SCALE(90), SCALE(124), SCALE(38), palette[1]);
            drawString("Check the Wii U's Internet settings.", SCALE(90), SCALE(162), SCALE(38), palette[1]);
            break;
        case SYNC_NO_PORT:
            drawString("Couldn't start syncing: port " + std::to_string(SaveSync::PORT) + " is in use.",
                SCALE(90), SCALE(124), SCALE(38), palette[1]);
            break;
        case SYNC_READY:
            drawString("Start Maelle Sync on the DSi. If it doesn't find", SCALE(90), SCALE(124), SCALE(38),
                palette[1]);
            drawString("this Wii U, enter this address there:", SCALE(90), SCALE(162), SCALE(38), palette[1]);
            drawString(address, SCALE(90), SCALE(210), SCALE(56), palette[1]);
            for (size_t i = 0; i < log.size(); i++)
                drawString(log[i], SCALE(90), SCALE(296 + i * 38), SCALE(32), palette[1]);
            break;
        }

        // Leave with B, or by tapping Back
        bool leave = getInputPress() & defaultKeys[INPUT_B];
        MenuTouch touch = getInputTouch();
        if (touch.pressed) {
            if (!touchStarted) {
                touchStart = touch;
                touchStarted = true;
                touchMode = true;
            }
        }
        else {
            if (touchStarted && touchStart.y >= 650 && touchStart.x >= boundsB) leave = true;
            touchStarted = false;
        }
        endFrame();
        if (leave) break;
    }

    // Stop the server, which ends a session within a moment, and wait for its thread
    syncQuit.store(true);
    thread->join();
    delete thread;
}
