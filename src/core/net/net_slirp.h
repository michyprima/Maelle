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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

class WorkerThread;

#ifdef NET_LOG
// Broadcast a line about network activity to UDP port 4405, for debugging on the console
void netLog(const char *format, ...);
void netLogFrame(const char *direction, const uint8_t *frame, size_t size);
#else
// Disabled logs name their arguments inside sizeof, which never runs them, so they're still checked and count as used
int netLogFrameArgs(const char *direction, const uint8_t *frame, size_t size);
#define netLog(...) ((void)sizeof(printf(__VA_ARGS__)))
#define netLogFrame(...) ((void)sizeof(netLogFrameArgs(__VA_ARGS__)))
#endif
struct Slirp;
struct pollfd;

// A virtual network behind the emulated access point, which turns the DS's traffic into the host's sockets
// libslirp runs on its own thread, and Ethernet frames cross to and from it through locked queues
class NetSlirp {
public:
    NetSlirp();
    ~NetSlirp();

    void send(const uint8_t *frame, size_t size);
    bool receive(std::vector<uint8_t> &frame);

private:
    struct Timer {
        void (*callback)(void*);
        void *opaque;
        int64_t expire;
    };

    WorkerThread *thread = nullptr;
    std::atomic<bool> running{true};
    Slirp *slirp = nullptr;

    std::mutex mutex;
    std::deque<std::vector<uint8_t>> toNet, fromNet;
    std::vector<pollfd> *polls = nullptr;
    std::vector<Timer*> timers;

    void run();

    // libslirp's callbacks, which use its types
    friend struct SlirpGlue;
};
