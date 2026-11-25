/*
    Copyright 2026 michyprima
*/

#include "sync_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

namespace SaveSync {

// How long a session waits for the DSi's next command, and for the rest of a command or save once it has started
static const int IDLE_TIMEOUT_MS = 10 * 60 * 1000, DATA_TIMEOUT_MS = 20 * 1000;
static const int SLEEP_MS = 5;

static void sleepBriefly(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void setNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
}

bool Server::start() {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int yes = 1;

    tcp = socket(AF_INET, SOCK_STREAM, 0);
    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (tcp < 0 || udp < 0) { stop(); return false; }
    setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    // Some network stacks, possibly the Wii U's, only hand broadcasts to sockets that allow them
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    if (bind(tcp, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(tcp, 1) < 0 ||
            bind(udp, (sockaddr*)&addr, sizeof(addr)) < 0) {
        stop();
        return false;
    }
    setNonBlocking(tcp);
    setNonBlocking(udp);
    say("listening on port " + std::to_string(PORT));
    return true;
}

void Server::stop() {
    if (tcp >= 0) close(tcp);
    if (udp >= 0) close(udp);
    if (pending >= 0) close(pending);
    tcp = udp = pending = -1;
}

int Server::acceptOne() {
    sockaddr_in from = {};
    socklen_t len = sizeof(from);
    int sock = accept(tcp, (sockaddr*)&from, &len);
    if (sock < 0) return -1;
    // Accepted sockets don't take on the listener's non-blocking mode everywhere
    setNonBlocking(sock);
    say(std::string("connection from ") + inet_ntoa(from.sin_addr));
    return sock;
}

void Server::poll(int timeoutMs) {
    for (int waited = 0; !stopping(); waited += SLEEP_MS) {
        answerDiscovery();
        int sock = (pending >= 0) ? pending : acceptOne();
        pending = -1;
        if (sock >= 0) {
            serve(sock);
            return;
        }
        if (waited >= timeoutMs) return;
        sleepBriefly(SLEEP_MS);
    }
}

void Server::serve(int sock) {
    session(sock);
    close(sock);
    say("session ended");
}

void Server::answerDiscovery() {
    // The Wii U refuses to receive a datagram into a small buffer, whatever its size; it's aligned like the buffer
    // Maelle's Wi-Fi network receives into, as the console's I/O processor reads and writes it directly
    alignas(0x40) static char buf[8192];
    while (true) {
        sockaddr_in from = {};
        socklen_t len = sizeof(from);
        ssize_t n = recvfrom(udp, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
        if (n < 0) {
            // Show each new failure once, since waiting for a datagram fails the same way every few milliseconds
            if (!wouldBlock(n) && errno != lastUdpError)
                say("discovery receive failed, error " + std::to_string(errno));
            lastUdpError = wouldBlock(n) ? 0 : errno;
            return;
        }
        if (n >= 7 && !memcmp(buf, "MAELLE?", 7)) {
            std::string reply = "MAELLE! " + id + " " + name;
            sendto(udp, reply.data(), reply.size(), 0, (sockaddr*)&from, len);
            // The DSi asks several ways at once, so each search only shows once
            auto now = std::chrono::steady_clock::now();
            if (from.sin_addr.s_addr != lastFinder || now - lastFound > std::chrono::seconds(5))
                say(std::string("found by ") + inet_ntoa(from.sin_addr));
            lastFinder = from.sin_addr.s_addr;
            lastFound = now;
        }
    }
}

bool Server::waitForCommand(Reader &in) {
    // Someone may spend a while on the DSi's list before syncing, so this waits much longer than for the rest of a
    // save. Meanwhile discovery is still answered, and a new connection takes over, so a DSi switched off or
    // restarted on its list doesn't keep the next one waiting
    for (int waited = 0; waited < IDLE_TIMEOUT_MS && !stopping(); waited += SLEEP_MS) {
        if (in.ready() != 0) return true;
        answerDiscovery();
        if ((pending = acceptOne()) >= 0) {
            say("a new connection takes over");
            return false;
        }
        sleepBriefly(SLEEP_MS);
    }
    return false;
}

bool Server::readSave(const std::string &path, std::vector<uint8_t> &data) {
    if (lockFiles) lockFiles();
    bool ok = readFile(path, data);
    if (unlockFiles) unlockFiles();
    return ok;
}

bool Server::writeSave(const std::string &path, const uint8_t *data, size_t size) {
    if (lockFiles) lockFiles();
    bool ok = replaceFile(path, data, size);
    if (unlockFiles) unlockFiles();
    return ok;
}

void Server::session(int sock) {
    // Give up on a DSi that stops answering mid-transfer, such as one switched off, or when asked to stop
    Wait wait = [this](int tries) {
        if (stopping() || tries * SLEEP_MS >= DATA_TIMEOUT_MS) return false;
        sleepBriefly(SLEEP_MS);
        return true;
    };
    Reader in(sock, wait);
    std::string line;
    if (!in.line(line)) return;
    if (line != "MAELLESYNC " + std::to_string(PROTOCOL)) {
        sendLine(sock, "ERR This DSi app and Maelle are different versions", wait);
        say("refused: " + line);
        return;
    }
    if (busy && busy()) {
        sendLine(sock, "ERR Close the game in Maelle first", wait);
        say("refused: busy");
        return;
    }
    if (!sendLine(sock, "MAELLESYNC " + std::to_string(PROTOCOL) + " " + id + " " + name, wait)) return;

    // Games are looked up once per session, so keys stay the same throughout it
    std::map<std::string, ServerSave> games;
    while (waitForCommand(in) && in.line(line)) {
        char key[64] = {};
        unsigned long size = 0;

        if (line == "LIST") {
            games.clear();
            std::vector<ServerSave> list = listSaves ? listSaves() : std::vector<ServerSave>();
            std::string reply;
            int count = 0;
            for (ServerSave &game : list) {
                // The first ROM with a key wins, as the DSi app does with duplicates
                if (games.count(game.rom.key)) continue;
                std::vector<uint8_t> data;
                bool has = readSave(game.savePath, data);
                reply += game.rom.key + " " + hex64(has ? hashSave(data.data(), data.size()) : 0) + " " +
                    std::to_string(has ? data.size() : 0) + " " + game.rom.title + "\n";
                games[game.rom.key] = game;
                count++;
            }
            if (!sendLine(sock, "LIST " + std::to_string(count), wait) ||
                    !sendAll(sock, reply.data(), reply.size(), wait))
                return;
            say("listed " + std::to_string(count) + " games");
        }
        else if (sscanf(line.c_str(), "GET %63s", key) == 1) {
            auto it = games.find(key);
            std::vector<uint8_t> data;
            if (it == games.end() || !readSave(it->second.savePath, data)) {
                if (!sendLine(sock, "ERR No save for this game", wait)) return;
                continue;
            }
            if (!sendLine(sock, "DATA " + std::to_string(data.size()), wait) ||
                    !sendAll(sock, data.data(), data.size(), wait))
                return;
            say("sent " + it->second.rom.title + " to the DSi");
        }
        else if (sscanf(line.c_str(), "PUT %63s %lu", key, &size) == 2) {
            if (size == 0 || size > MAX_SAVE) {
                sendLine(sock, "ERR Bad save size", wait);
                return; // The data that follows can't be skipped safely
            }
            std::vector<uint8_t> data(size);
            if (!in.bytes(data.data(), size)) return;

            auto it = games.find(key);
            if (it == games.end()) {
                if (!sendLine(sock, "ERR This game isn't here", wait)) return;
                continue;
            }

            // Keep the size of the save this side already has when the content fits it
            std::vector<uint8_t> old;
            size_t oldSize = readSave(it->second.savePath, old) ? old.size() : 0;
            data.resize(targetSize(contentSize(data.data(), size), oldSize, size), 0xFF);
            if (!writeSave(it->second.savePath, data.data(), data.size())) {
                if (!sendLine(sock, "ERR Couldn't write the save", wait)) return;
                say("couldn't write " + it->second.savePath);
                continue;
            }
            if (!sendLine(sock, "OK " + hex64(hashSave(data.data(), data.size())), wait)) return;
            say("received " + it->second.rom.title + " from the DSi");
        }
        else if (line == "BYE") {
            return;
        }
        else {
            sendLine(sock, "ERR Unknown command", wait);
            return;
        }
    }
}

} // namespace SaveSync
