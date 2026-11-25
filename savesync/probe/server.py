#!/usr/bin/env python3
# Copyright 2026 michyprima
#
# Test server for the save sync probe: answers discovery broadcasts and echoes the probe's data block back.
# Run on a computer on the same network as the DSi: python3 server.py

import socket
import struct
import threading
import time

PORT = 5821


def discovery():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    while True:
        data, addr = s.recvfrom(64)
        if data.startswith(b"MAELLE?"):
            print(f"discovery from {addr[0]}", flush=True)
            s.sendto(b"MAELLE!", addr)


def recv_exact(conn, size):
    data = bytearray()
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            raise ConnectionError("closed early")
        data += chunk
    return bytes(data)


def client(conn, addr):
    with conn:
        hello = b""
        while not hello.endswith(b"\n"):
            hello += recv_exact(conn, 1)
        size = struct.unpack("<I", recv_exact(conn, 4))[0]
        start = time.time()
        block = recv_exact(conn, size)
        up = time.time() - start
        conn.sendall(block)
        print(f"{addr[0]}: {hello.decode().strip()}, {size // 1024} KB in {up:.2f} s, echoed", flush=True)


def main():
    threading.Thread(target=discovery, daemon=True).start()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    s.listen()
    print(f"listening on port {PORT}", flush=True)
    while True:
        conn, addr = s.accept()
        threading.Thread(target=client, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    main()
