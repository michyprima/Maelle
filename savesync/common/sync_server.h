/*
    Copyright 2026 michyprima

    The side of save sync that holds the emulator's saves: answers the DSi app's discovery broadcasts and serves one
    sync session at a time over TCP. Protocol, one text line per command, with save data sent raw after its line:

      DSi: MAELLESYNC <version>          Server: MAELLESYNC <version> <id> <name>, or ERR <reason>
      DSi: LIST                          Server: LIST <n>, then n lines of <key> <hash> <size> <title>
      DSi: GET <key>                     Server: DATA <size> and the save, or ERR <reason>
      DSi: PUT <key> <size> and the save Server: OK <hash>, or ERR <reason>
      DSi: BYE

    Discovery: the DSi broadcasts "MAELLE?" over UDP to PORT, and the server answers "MAELLE! <id> <name>".

    Its sockets don't block, and waits are short sleeps, since the Wii U's poll() and select() fail with sockets numbered
    above 31; that also lets another thread stop it within a moment, even mid-session.
*/

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "sync_core.h"
#include "sync_net.h"

namespace SaveSync {

struct ServerSave {
    RomInfo rom;
    std::string savePath; // Where this game's save is, or will be written if there isn't one yet
};

class Server {
public:
    std::function<std::vector<ServerSave>()> listSaves; // The games this side has, looked up for each session
    std::function<bool()> busy;                         // True while saves can't be touched, such as during a game
    std::function<void(const std::string&)> log;
    std::function<void()> lockFiles, unlockFiles; // Taken around each save read or written, if set
    std::string id, name;
    std::function<bool()> stopRequested; // Checked often, to end poll() and any session soon once it's true

    ~Server() { stop(); }
    bool start();
    void stop();

    // Answer discovery and handle a session if one comes in within the timeout; a session runs to its end here
    void poll(int timeoutMs);

private:
    int tcp = -1, udp = -1;
    uint32_t lastFinder = 0; // The last DSi that found this server, and when, so each search is shown once
    std::chrono::steady_clock::time_point lastFound;
    int lastUdpError = 0; // The last failure receiving discovery, shown once
    int pending = -1; // A connection that came in during a session, which takes over once it ends
    void session(int sock);
    void serve(int sock);
    void answerDiscovery();
    int acceptOne();
    bool waitForCommand(Reader &in);
    bool readSave(const std::string &path, std::vector<uint8_t> &data);
    bool writeSave(const std::string &path, const uint8_t *data, size_t size);
    bool stopping() const { return stopRequested && stopRequested(); }
    void say(const std::string &text) { if (log) log(text); }
};

} // namespace SaveSync
