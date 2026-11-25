/*
    Copyright 2026 michyprima

    Socket helpers shared by both ends of save sync: buffered reading of command lines and data, and sending them.
*/

#pragma once

#include <sys/socket.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace SaveSync {

// For non-blocking sockets: called when a socket has nothing to read or no room to send yet, with how many times in a
// row that happened; it waits a little and returns true to try again, or false to give up. Blocking sockets never need it
typedef std::function<bool(int)> Wait;

inline bool wouldBlock(int result) {
    return result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

class Reader {
public:
    explicit Reader(int sock, Wait wait = nullptr): sock(sock), wait(wait) {}

    bool line(std::string &text) {
        text.clear();
        while (true) {
            for (; pos < end; pos++) {
                if (buf[pos] == '\n') { pos++; return true; }
                if (text.size() > 1024) return false;
                text += buf[pos];
            }
            if (!fill()) return false;
        }
    }

    // Whether data already read from the socket is waiting here
    bool buffered() const { return pos < end; }

    // For a non-blocking socket, check for data without waiting: 1 if some is here, 0 if not yet, -1 if the socket
    // closed or failed, which reading then reports
    int ready() {
        if (pos < end) return 1;
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) { pos = 0; end = n; return 1; }
        return wouldBlock(n) ? 0 : -1;
    }

    bool bytes(uint8_t *data, size_t size) {
        while (size > 0) {
            if (pos == end && !fill()) return false;
            size_t n = std::min(size, end - pos);
            memcpy(data, buf + pos, n);
            data += n; pos += n; size -= n;
        }
        return true;
    }

private:
    int sock;
    Wait wait;
    char buf[4096];
    size_t pos = 0, end = 0;

    bool fill() {
        for (int tries = 0;; tries++) {
            int n = recv(sock, buf, sizeof(buf), 0);
            if (wouldBlock(n) && wait && wait(tries)) continue;
            if (n <= 0) return false;
            pos = 0; end = n;
            return true;
        }
    }
};

inline bool sendAll(int sock, const void *data, size_t size, const Wait &wait = nullptr) {
    const uint8_t *p = (const uint8_t*)data;
    int tries = 0;
    while (size > 0) {
        int n = send(sock, p, size, 0);
        if (wouldBlock(n) && wait && wait(tries++)) continue;
        if (n <= 0) return false;
        p += n; size -= n;
        tries = 0;
    }
    return true;
}

inline bool sendLine(int sock, const std::string &text, const Wait &wait = nullptr) {
    std::string line = text + "\n";
    return sendAll(sock, line.data(), line.size(), wait);
}

} // namespace SaveSync
