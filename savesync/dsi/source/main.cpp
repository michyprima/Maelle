/*
    Copyright 2026 michyprima

    Maelle Sync: syncs the DSi's game saves (as TWiLight Menu++ and nds-bootstrap keep them) with Maelle's over the
    home network. Finds ROMs on the SD card, asks Maelle for its list, shows which way each save will go, then sends
    and receives them, keeping each replaced save as .sav.bak.
*/

#include <nds.h>
#include <dswifi9.h>
#include <fat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include "../../common/sync_core.h"
#include "../../common/sync_net.h"

using namespace SaveSync;

static const char *STATE_PATH = "sd:/_nds/maelle-sync.txt";

struct LocalGame {
    RomInfo rom;
    std::string savePath; // The existing save, or where a received one goes
    bool hasSave;
    size_t size;
    uint64_t hash;
};

struct RemoteGame {
    RomInfo rom;
    uint64_t hash;
    size_t size;
};

struct Entry {
    int local, remote;
    Action suggested, action;
    bool hasBase;
};

static PrintConsole top, bottom;
static std::vector<LocalGame> locals;
static std::vector<RemoteGame> remotes;
static std::vector<Entry> entries;
static int onlyHere, onlyThere;

// Saved between runs: the last server address, and for each server the hashes both sides had at the last sync
static std::string lastIp;
static std::vector<std::string> otherLines; // Lines for other servers, kept as they are
static std::map<std::string, uint64_t> bases;
static std::string serverId, serverName;
static std::string basesId; // The server the bases are for

// Screens

static void clearTop() { consoleSelect(&top); consoleClear(); }
static void clearBottom() { consoleSelect(&bottom); consoleClear(); }

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...) {
    consoleSelect(&bottom);
    va_list args;
    va_start(args, fmt);
    viprintf(fmt, args);
    va_end(args);
}

static bool frame() {
    swiWaitForVBlank();
    scanKeys();
    return pmMainLoop();
}

// Wait for A (true) or B/START (false)
static bool askAB() {
    while (frame()) {
        u32 keys = keysDown();
        if (keys & KEY_A) return true;
        if (keys & (KEY_B | KEY_START)) return false;
    }
    return false;
}

// State file

static void loadState() {
    FILE *file = fopen(STATE_PATH, "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "ip ", 3)) lastIp = line + 3;
        else if (line[0]) otherLines.push_back(line);
    }
    fclose(file);
}

// Pick out the bases for the server just connected to; the rest stay as they were
static void selectBases() {
    // Put back the bases of the server picked before, as a later round in the same run may reach another one
    for (auto &base : bases)
        otherLines.push_back("base " + basesId + " " + base.first + " " + hex64(base.second));
    bases.clear();
    basesId = serverId;
    std::vector<std::string> keep;
    for (const std::string &line : otherLines) {
        char id[32], key[64], hash[32];
        if (sscanf(line.c_str(), "base %31s %63s %31s", id, key, hash) == 3 && serverId == id)
            bases[key] = parseHex64(hash);
        else
            keep.push_back(line);
    }
    otherLines = keep;
}

static bool saveState() {
    std::string text = "ip " + lastIp + "\n";
    for (const std::string &line : otherLines) text += line + "\n";
    for (auto &base : bases)
        text += "base " + basesId + " " + base.first + " " + hex64(base.second) + "\n";
    mkdir("sd:/_nds", 0777);
    return replaceFile(STATE_PATH, (const uint8_t*)text.data(), text.size());
}

// Finding games on the SD card

static bool hasExtension(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static bool exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool isDir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void scanDir(const std::string &dir, int depth) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs, roms;
    bool savesInDir = false;
    while (dirent *e = readdir(d)) {
        const char *name = e->d_name;
        if (name[0] == '.') continue;
        if (e->d_type == DT_DIR) {
            // System folders, TWiLight's own folder and photos hold no games
            static const char *skip[] = { "_nds", "private", "title", "ticket", "shared1", "shared2", "sys", "tmp",
                "DCIM", "photo", "Nintendo 3DS", "System Volume Information", "saves" };
            bool skipped = false;
            for (const char *s : skip) skipped |= !strcasecmp(name, s);
            if (!skipped && depth < 5) subdirs.push_back(name);
        }
        else if (hasExtension(name, ".nds") || hasExtension(name, ".dsi")) {
            roms.push_back(name);
        }
        else if (hasExtension(name, ".sav")) {
            savesInDir = true;
        }
    }
    closedir(d);

    std::string base = (dir.back() == '/') ? dir : dir + "/";
    for (const std::string &name : roms) {
        std::string path = base + name;
        uint8_t header[HEADER_SIZE];
        FILE *file = fopen(path.c_str(), "rb");
        if (!file) continue;
        bool ok = fread(header, 1, HEADER_SIZE, file) == HEADER_SIZE;
        fclose(file);
        LocalGame game = {};
        if (!ok || !readRomInfo(header, game.rom)) continue;

        // nds-bootstrap keeps saves in a saves folder next to the ROMs, or next to the ROM itself; with neither,
        // a received save goes where it would make one
        std::string stem = name.substr(0, name.rfind('.'));
        std::string inSaves = base + "saves/" + stem + ".sav", beside = base + stem + ".sav";
        if (exists(inSaves)) game.savePath = inSaves;
        else if (exists(beside)) game.savePath = beside;
        else game.savePath = (savesInDir && !isDir(base + "saves")) ? beside : inSaves;
        locals.push_back(game);
    }
    for (const std::string &sub : subdirs) scanDir(base + sub, depth + 1);
}

static void hashLocals() {
    std::vector<uint8_t> data;
    for (size_t i = 0; i < locals.size(); i++) {
        LocalGame &game = locals[i];
        status("\rReading saves %u/%u  ", (unsigned)i + 1, (unsigned)locals.size());
        game.hasSave = readFile(game.savePath, data);
        game.size = game.hasSave ? data.size() : 0;
        game.hash = game.hasSave ? hashSave(data.data(), data.size()) : 0;
    }
    status("\n");
}

// Network

static bool waitForWifi() {
    status("Joining the network ");
    for (int frames = 0; frames < 60 * 30; frames++) {
        if (Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED) {
            struct in_addr ip = { Wifi_GetIP() };
            status("ok\nDSi address %s\n", inet_ntoa(ip));
            return true;
        }
        if (frames % 30 == 0) status(".");
        if (!frame()) return false;
    }
    status(" failed\nCheck the connections in\nSystem Settings > Internet.\n");
    return false;
}

static unsigned long discover() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    ioctl(sock, FIONBIO, &yes);
    // Ask everywhere a server might hear: the all-ones broadcast, this network's own broadcast address, which some
    // stacks take when they drop the first, and the last address used, in case broadcasts don't get through at all
    struct in_addr gateway, mask, dns1, dns2;
    Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
    unsigned long targets[3] = { INADDR_BROADCAST, (unsigned long)(Wifi_GetIP() | ~mask.s_addr), 0 };
    int parts[4];
    if (sscanf(lastIp.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) == 4)
        targets[2] = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);

    unsigned long found = 0;
    for (int frames = 0; frames < 150 && !found; frames++) {
        if (frames % 30 == 0) {
            for (int i = 0; i < 3; i++) {
                if (!targets[i] || (i > 0 && targets[i] == targets[0])) continue;
                to.sin_addr.s_addr = targets[i];
                sendto(sock, "MAELLE?", 7, 0, (sockaddr*)&to, sizeof(to));
            }
        }
        char buf[128];
        sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &len);
        if (n >= 7 && !memcmp(buf, "MAELLE!", 7)) found = from.sin_addr.s_addr;
        if (!frame()) break;
    }
    closesocket(sock);

    // The emulator's virtual network answers from the host's loopback, which is the gateway seen from inside
    if (found && (ntohl(found) >> 24) == 127) {
        struct in_addr gateway;
        struct in_addr mask, dns1, dns2;
        Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
        found = gateway.s_addr;
    }
    return found;
}

// Returned by enterAddress to look for Maelle by itself again; the broadcast address is never a server's
static const unsigned long SEARCH_AGAIN = INADDR_BROADCAST;

// Let the address be typed in, for when broadcasts don't reach Maelle (another subnet, or blocked); 0 to quit
static unsigned long enterAddress() {
    int parts[4] = { 192, 168, 1, 2 };
    if (sscanf(lastIp.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) != 4) {
        struct in_addr gateway;
        struct in_addr mask, dns1, dns2;
        Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
        uint32_t g = ntohl(gateway.s_addr);
        for (int i = 0; i < 4; i++) parts[i] = (g >> (24 - i * 8)) & 0xFF;
    }
    int cursor = 3;
    keysSetRepeat(20, 4);
    while (true) {
        clearTop();
        iprintf("\n Maelle wasn't found by itself.\n\n Type the address shown on\n Maelle's sync screen:\n\n   ");
        for (int i = 0; i < 4; i++)
            iprintf(i == cursor ? "[%3d]" : " %3d ", parts[i]);
        iprintf("\n\n Left/Right: choose a number\n Up/Down: change it (L/R: 10)\n\n A: connect   Y: search again\n B: quit\n");
        while (true) {
            if (!frame()) return 0;
            u32 keys = keysDownRepeat();
            if (keys & KEY_A) {
                uint32_t ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
                return htonl(ip);
            }
            if (keys & KEY_B) return 0;
            if (keys & KEY_Y) return SEARCH_AGAIN;
            int step = (keys & KEY_UP) ? 1 : (keys & KEY_DOWN) ? -1 : (keys & KEY_R) ? 10 : (keys & KEY_L) ? -10 : 0;
            if (keys & KEY_LEFT) cursor = (cursor + 3) % 4;
            if (keys & KEY_RIGHT) cursor = (cursor + 1) % 4;
            if (step) parts[cursor] = (parts[cursor] + step + 256) % 256;
            if (keys & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN | KEY_L | KEY_R)) break;
        }
    }
}

static int connectTo(unsigned long addr) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);
    to.sin_addr.s_addr = addr;
    if (connect(sock, (sockaddr*)&to, sizeof(to)) < 0) {
        closesocket(sock);
        return -1;
    }
    return sock;
}

static bool greet(int sock, Reader &in) {
    std::string line;
    if (!sendLine(sock, "MAELLESYNC " + std::to_string(PROTOCOL)) || !in.line(line)) {
        status("No answer from Maelle.\n");
        return false;
    }
    if (!line.compare(0, 4, "ERR ")) {
        status("Maelle says: %s\n", line.c_str() + 4);
        return false;
    }
    int version;
    char id[32];
    int nameAt = 0;
    if (sscanf(line.c_str(), "MAELLESYNC %d %31s %n", &version, id, &nameAt) < 2 || version != PROTOCOL) {
        status("Unexpected answer from Maelle.\n");
        return false;
    }
    serverId = id;
    serverName = nameAt ? line.substr(nameAt) : "Maelle";
    return true;
}

static bool fetchList(int sock, Reader &in) {
    std::string line;
    int count;
    if (!sendLine(sock, "LIST") || !in.line(line) || sscanf(line.c_str(), "LIST %d", &count) != 1) return false;
    remotes.clear();
    for (int i = 0; i < count; i++) {
        if (!in.line(line)) return false;
        char key[64], hash[32];
        unsigned long size;
        int titleAt = 0;
        if (sscanf(line.c_str(), "%63s %31s %lu %n", key, hash, &size, &titleAt) < 3) return false;
        RemoteGame game = {};
        game.rom.key = key;
        game.rom.code = std::string(key).substr(0, 4);
        game.rom.title = titleAt ? line.substr(titleAt) : key;
        game.hash = parseHex64(hash);
        game.size = size;
        remotes.push_back(game);
    }
    return true;
}

// Planning

static void plan() {
    std::vector<RomInfo> localRoms, remoteRoms;
    for (LocalGame &g : locals) localRoms.push_back(g.rom);
    for (RemoteGame &g : remotes) remoteRoms.push_back(g.rom);
    std::vector<int> pairs = pairGames(localRoms, remoteRoms);

    entries.clear();
    onlyHere = onlyThere = 0;
    std::vector<bool> paired(remotes.size(), false);
    std::map<std::string, bool> seen;
    for (size_t i = 0; i < locals.size(); i++) {
        if (pairs[i] < 0) {
            if (locals[i].hash) onlyHere++;
            continue;
        }
        paired[pairs[i]] = true;
        // The same ROM in two folders is one game; the first found is used
        if (seen[locals[i].rom.key]) continue;
        seen[locals[i].rom.key] = true;

        const LocalGame &l = locals[i];
        const RemoteGame &r = remotes[pairs[i]];
        if (!l.hash && !r.hash) continue; // No save on either side
        Entry e;
        e.local = i;
        e.remote = pairs[i];
        auto base = bases.find(r.rom.key);
        e.hasBase = base != bases.end();
        e.suggested = decide(l.hash, r.hash, e.hasBase, e.hasBase ? base->second : 0);
        // A conflict is only resolved by choosing, so it starts out skipped
        e.action = (e.suggested == Action::Conflict) ? Action::Skip : e.suggested;
        entries.push_back(e);
    }
    for (size_t i = 0; i < remotes.size(); i++)
        if (!paired[i] && remotes[i].hash) onlyThere++;

    // Conflicts first, then saves that move, then the rest, each by title
    auto rank = [](const Entry &e) {
        return e.suggested == Action::Conflict ? 0 : e.suggested == Action::Same ? 2 : 1;
    };
    std::sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return locals[a.local].rom.title < locals[b.local].rom.title;
    });
}

static const char *actionText(Action a) {
    switch (a) {
        case Action::Same: return " same  ";
        case Action::Upload: return "DSi->U ";
        case Action::Download: return "U->DSi ";
        case Action::Conflict: return "CHOOSE ";
        default: return " skip  ";
    }
}

static void describe(const Entry &e) {
    const LocalGame &l = locals[e.local];
    const RemoteGame &r = remotes[e.remote];
    clearBottom();
    iprintf("%s\n", l.rom.title.c_str());
    if (l.rom.key != r.rom.key) iprintf("(different versions: %s/%s)\n", l.rom.key.c_str(), r.rom.key.c_str());
    iprintf("DSi:    %s\n", l.hash ? (std::to_string(l.size / 1024) + " KB " + hex64(l.hash).substr(0, 8)).c_str() :
        "no save");
    iprintf("Maelle: %s\n", r.hash ? (std::to_string(r.size / 1024) + " KB " + hex64(r.hash).substr(0, 8)).c_str() :
        "no save");
    switch (e.suggested) {
        case Action::Same: iprintf("Both have the same save.\n"); break;
        case Action::Upload: iprintf("Changed on the DSi.\n"); break;
        case Action::Download: iprintf("Changed in Maelle.\n"); break;
        case Action::Conflict:
            iprintf(e.hasBase ? "\x1b[31mChanged on both sides since the\nlast sync. Choose one!\x1b[39m\n" :
                "\x1b[31mNever synced and different.\nChoose one!\x1b[39m\n");
            break;
        default: break;
    }
    iprintf("\nThe replaced save is kept\nas .sav.bak.\n");
    iprintf("\x1b[20;0HUp/Down: game  Left/Right: way\nA: sync now     B: quit\nY: read the saves and look again\n");
}

enum class Review { Sync, Quit, Nothing, Again };

static Review review() {
    if (entries.empty()) {
        clearTop();
        iprintf("\n No games with saves on both\n sides to sync.\n\n With a save on the DSi only: %d\n In Maelle only: %d\n",
            onlyHere, onlyThere);
        clearBottom();
        return Review::Nothing;
    }

    int cursor = 0, scroll = 0;
    const int rows = 20;
    keysSetRepeat(20, 4);
    while (true) {
        int moving = 0, choose = 0;
        for (Entry &e : entries) {
            moving += (e.action == Action::Upload || e.action == Action::Download);
            choose += (e.suggested == Action::Conflict && e.action == Action::Skip);
        }
        clearTop();
        iprintf("Sync with %.20s\n", serverName.c_str());
        iprintf("%d to sync, %d to choose\n", moving, choose);
        iprintf("--------------------------------");
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + rows) scroll = cursor - rows + 1;
        for (int i = scroll; i < (int)entries.size() && i < scroll + rows; i++) {
            const Entry &e = entries[i];
            bool red = e.suggested == Action::Conflict && e.action == Action::Skip;
            // The game code tells regions apart, such as AMCE and AMCP for Mario Kart DS
            iprintf("%s%c%s%s %.12s%s\n", red ? "\x1b[31m" : "", i == cursor ? '>' : ' ',
                (red ? "CHOOSE " : actionText(e.action)), locals[e.local].rom.code.c_str(),
                locals[e.local].rom.title.c_str(), red ? "\x1b[39m" : "");
        }
        // Saves of games the other side doesn't have can't be synced; say how many, so they aren't a mystery
        if (onlyHere || onlyThere)
            iprintf("\x1b[23;0HOnly on DSi: %d  Only on U: %d", onlyHere, onlyThere);
        describe(entries[cursor]);

        while (true) {
            if (!frame()) return Review::Quit;
            u32 keys = keysDownRepeat();
            Entry &e = entries[cursor];
            if ((keys & KEY_A) && moving) return Review::Sync;
            if (keys & KEY_B) return Review::Quit;
            if (keys & KEY_Y) return Review::Again;
            if (keys & KEY_UP) cursor = (cursor + entries.size() - 1) % entries.size();
            if (keys & KEY_DOWN) cursor = (cursor + 1) % entries.size();
            if ((keys & (KEY_LEFT | KEY_RIGHT)) && e.suggested != Action::Same) {
                // Cycle through the ways a save can go; a blank or missing save is never sent over one with data
                std::vector<Action> ways = { Action::Skip };
                if (locals[e.local].hash) ways.push_back(Action::Upload);
                if (remotes[e.remote].hash) ways.push_back(Action::Download);
                size_t at = std::find(ways.begin(), ways.end(), e.action) - ways.begin();
                at = (keys & KEY_RIGHT) ? (at + 1) % ways.size() : (at + ways.size() - 1) % ways.size();
                e.action = ways[at];
            }
            if (keys & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) break;
        }
    }
}

// Syncing

static bool upload(int sock, Reader &in, Entry &e) {
    LocalGame &l = locals[e.local];
    std::vector<uint8_t> data;
    std::string line;
    if (!readFile(l.savePath, data)) {
        status("can't read\n");
        return false;
    }
    if (!sendLine(sock, "PUT " + remotes[e.remote].rom.key + " " + std::to_string(data.size())) ||
            !sendAll(sock, data.data(), data.size()) || !in.line(line)) {
        status("connection lost\n");
        return false;
    }
    if (line.compare(0, 3, "OK ")) {
        status("%s\n", line.c_str());
        return false;
    }
    // Maelle answers with the hash of what it wrote, so a save damaged on the way shows up here
    uint64_t hash = hashSave(data.data(), data.size());
    if (parseHex64(line.c_str() + 3) != hash) {
        status("arrived damaged\n");
        return false;
    }
    bases[remotes[e.remote].rom.key] = hash;
    return true;
}

static bool download(int sock, Reader &in, Entry &e) {
    LocalGame &l = locals[e.local];
    RemoteGame &r = remotes[e.remote];
    std::string line;
    unsigned long size;
    if (!sendLine(sock, "GET " + r.rom.key) || !in.line(line)) {
        status("connection lost\n");
        return false;
    }
    if (sscanf(line.c_str(), "DATA %lu", &size) != 1 || size == 0 || size > MAX_SAVE) {
        status("%s\n", line.c_str());
        return false;
    }
    std::vector<uint8_t> data(size);
    if (!in.bytes(data.data(), size)) {
        status("connection lost\n");
        return false;
    }
    if (hashSave(data.data(), size) != r.hash) {
        status("changed in Maelle meanwhile\n");
        return false;
    }

    // Keep the size of the save the DSi already has when the content fits it, as nds-bootstrap made it
    data.resize(targetSize(contentSize(data.data(), size), l.hasSave ? l.size : 0, size), 0xFF);
    std::string dir = l.savePath.substr(0, l.savePath.rfind('/'));
    mkdir(dir.c_str(), 0777);
    if (!replaceFile(l.savePath, data.data(), data.size())) {
        status("can't write the SD card\n");
        return false;
    }
    bases[r.rom.key] = r.hash;
    return true;
}

static void runSync(int sock, Reader &in) {
    clearBottom();
    int done = 0, failed = 0;
    for (Entry &e : entries) {
        if (e.action == Action::Same) {
            // Remember the agreed save, so a later change on one side is clearly that side's
            if (locals[e.local].hash) bases[remotes[e.remote].rom.key] = locals[e.local].hash;
            continue;
        }
        if (e.action != Action::Upload && e.action != Action::Download) continue;
        status("%s %.20s ", e.action == Action::Upload ? "->U" : "<-U", locals[e.local].rom.title.c_str());
        bool ok = (e.action == Action::Upload) ? upload(sock, in, e) : download(sock, in, e);
        if (ok) { done++; status("ok\n"); }
        else failed++;
        // Save progress as it goes, so a sync cut short still remembers what got through
        saveState();
    }
    saveState();
    sendLine(sock, "BYE");
    status("\nDone: %d synced, %d failed.\n", done, failed);
}

int main(int argc, char **argv) {
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&top, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottom, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    clearTop();
    iprintf("\n  Maelle Sync\n\n  Syncs this DSi's saves with\n  Maelle on the Wii U.\n\n"
        "  Open Maelle's sync screen,\n  then wait here.\n");
    if (!isDSiMode()) {
        status("Needs DSi mode, to use the\nSD card and the DSi's Wi-Fi.\n");
        askAB();
        return 0;
    }

    // Join the network while the SD card is searched
    Wifi_InitDefault(INIT_ONLY);
    Wifi_AutoConnect();

    if (!fatInitDefault()) {
        status("Can't open the SD card.\n");
        askAB();
        return 0;
    }
    loadState();
    status("Looking for games...\n");
    scanDir("sd:/", 0);
    status("%u games found\n", (unsigned)locals.size());
    hashLocals();
    if (!waitForWifi()) {
        askAB();
        return 0;
    }

    while (true) {
        status("Looking for Maelle...");
        unsigned long addr = discover();
        status(addr ? " found\n" : " not found\n");
        if (!addr && !(addr = enterAddress())) return 0;
        if (addr == SEARCH_AGAIN) continue;

        struct in_addr a = { addr };
        status("Connecting to %s\n", inet_ntoa(a));
        int sock = connectTo(addr);
        if (sock < 0) {
            status("Couldn't connect.\nA: try again  B: quit\n");
            if (!askAB()) return 0;
            continue;
        }
        Reader in(sock);
        bool ok = greet(sock, in);
        if (ok) {
            lastIp = inet_ntoa(a);
            selectBases();
            ok = fetchList(sock, in);
            if (!ok) status("Couldn't get Maelle's list.\n");
        }
        Review choice = Review::Nothing;
        if (ok) {
            plan();
            choice = review();
            if (choice == Review::Sync) {
                runSync(sock, in);
            } else {
                sendLine(sock, "BYE");
                if (choice == Review::Quit) {
                    closesocket(sock);
                    return 0;
                }
            }
        }
        closesocket(sock);

        // Read the saves again, so another round starts from what's on the card now
        if (!(ok && choice == Review::Again)) {
            status("A: sync again  B: quit\n");
            if (!askAB()) return 0;
        }
        for (LocalGame &g : locals) {
            std::vector<uint8_t> data;
            g.hasSave = readFile(g.savePath, data);
            g.size = g.hasSave ? data.size() : 0;
            g.hash = g.hasSave ? hashSave(data.data(), data.size()) : 0;
        }
        clearBottom();
    }
}
