#!/usr/bin/env python3
# Copyright 2026 michyprima
#
# Checks the save sync server's answers to bad or unusual requests. Run against a server started on a fixture from
# make_fixture.py: protocol_test.py [host]

import socket
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 5821
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    failures += not cond


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        self.buf = b""

    def send(self, data):
        self.s.sendall(data)

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()

    def data(self, size):
        while len(self.buf) < size:
            chunk = self.s.recv(65536)
            if not chunk:
                return None
            self.buf += chunk
        out, self.buf = self.buf[:size], self.buf[size:]
        return out


# Discovery answers with the ID and name
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.settimeout(2)
u.sendto(b"MAELLE?", (HOST, PORT))
reply = u.recv(128).decode()
check(reply.startswith("MAELLE! ") and len(reply.split()) >= 3, f"discovery: {reply}")

# Another version is refused
c = Conn()
c.send(b"MAELLESYNC 99\n")
check(c.line().startswith("ERR "), "other version refused")

# Normal session
c = Conn()
c.send(b"MAELLESYNC 1\n")
hello = c.line()
check(hello.startswith("MAELLESYNC 1 "), f"greeting: {hello}")
c.send(b"LIST\n")
n = int(c.line().split()[1])
games = {}
for _ in range(n):
    key, h, size, title = c.line().split(" ", 3)
    games[title] = (key, h, int(size))
check("GAME H" in games and "GAME E" not in games, f"list has {n} games")

# A game it doesn't have, before listing it isn't possible; unknown keys are refused but the session goes on
c.send(b"GET NONE-0000\n")
check(c.line().startswith("ERR "), "GET unknown key refused")
c.send(b"PUT NONE-0000 4\n\x01\x02\x03\x04")
check(c.line().startswith("ERR "), "PUT unknown key refused, data skipped")
c.send(b"GET " + games["GAME H"][0].encode() + b"\n")
line = c.line()
size = int(line.split()[1])
check(line.startswith("DATA ") and size == games["GAME H"][2] and len(c.data(size)) == size, "GET still works after")

# Sending the save it just sent back changes nothing but makes a .bak
data = None
c.send(b"GET " + games["GAME H"][0].encode() + b"\n")
size = int(c.line().split()[1])
data = c.data(size)
c.send(b"PUT " + games["GAME H"][0].encode() + b" " + str(size).encode() + b"\n" + data)
ok = c.line()
check(ok == "OK " + games["GAME H"][1], f"PUT same save gives the same hash: {ok}")

# A zero size can't be followed safely, so the session ends
c.send(b"PUT " + games["GAME H"][0].encode() + b" 0\n")
check(c.line().startswith("ERR ") and c.line() is None, "PUT size 0 refused and closed")

# Unknown commands end the session
c = Conn()
c.send(b"MAELLESYNC 1\n")
c.line()
c.send(b"HELLO\n")
check(c.line().startswith("ERR ") and c.line() is None, "unknown command refused and closed")

print(f"{failures} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
