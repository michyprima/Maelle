#!/usr/bin/env python3
# Copyright 2026 michyprima
#
# Makes a save sync test setup: fake ROMs (headers only) and saves for both sides, covering each way a save can go.
# Usage: make_fixture.py <output folder>
# Writes <out>/dsi (the DSi's SD card contents, also as <out>/sd.img if macOS tools are there) and <out>/maelle.

import os
import shutil
import subprocess
import sys


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def rom(title, code, version=0):
    header = bytearray(0x200)
    header[0:len(title)] = title.encode()
    header[0x0C:0x10] = code.encode()
    header[0x12] = 2  # DSi-enhanced, like many late games; only the header is read anyway
    header[0x1E] = version
    crc = crc16(header[:0x15E])
    header[0x15E] = crc & 0xFF
    header[0x15F] = crc >> 8
    return bytes(header)


def save(seed, size, content=4096):
    data = bytearray(b"\xff" * size)
    for i in range(content):
        data[i] = (i * seed + seed * 7) & 0xFF
    return bytes(data)


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main():
    out = sys.argv[1]
    dsi, maelle = os.path.join(out, "dsi"), os.path.join(out, "maelle")
    for d in (dsi, maelle):
        if os.path.exists(d):
            shutil.rmtree(d)
    nds, roms = os.path.join(dsi, "roms", "nds"), os.path.join(maelle, "roms")
    saves = os.path.join(nds, "saves")
    k = 1024

    # A: save only on the DSi -> goes to Maelle
    put(f"{nds}/Game A.nds", rom("GAME A", "AAAE")); put(f"{saves}/Game A.sav", save(3, 512 * k))
    put(f"{roms}/Game A.nds", rom("GAME A", "AAAE"))
    # B: save only in Maelle -> comes to the DSi, into its saves folder
    put(f"{nds}/Game B.nds", rom("GAME B", "BBBE"))
    put(f"{roms}/Game B.nds", rom("GAME B", "BBBE")); put(f"{roms}/Game B.sav", save(5, 64 * k))
    # C: same save, 512 KB on the DSi and 64 KB in Maelle -> same
    put(f"{nds}/Game C.nds", rom("GAME C", "CCCE")); put(f"{saves}/Game C.sav", save(7, 512 * k))
    put(f"{roms}/Game C.nds", rom("GAME C", "CCCE")); put(f"{roms}/Game C.sav", save(7, 64 * k))
    # D: different saves, never synced -> conflict
    put(f"{nds}/Game D.nds", rom("GAME D", "DDDE")); put(f"{saves}/Game D.sav", save(9, 256 * k))
    put(f"{roms}/Game D.nds", rom("GAME D", "DDDE")); put(f"{roms}/Game D.sav", save(11, 256 * k))
    # E: only on the DSi
    put(f"{nds}/Game E.nds", rom("GAME E", "EEEE")); put(f"{saves}/Game E.sav", save(13, 8 * k))
    # F: two versions of a game (headers differ), save on the DSi, in a subfolder with the save beside the ROM
    put(f"{dsi}/games/Game F.nds", rom("GAME F", "FFFE", 0)); put(f"{dsi}/games/Game F.sav", save(15, 128 * k))
    put(f"{roms}/Game F (Rev 1).nds", rom("GAME F", "FFFE", 1))
    # G: blank 512 KB save on the DSi (as a fresh one), real one in Maelle -> comes to the DSi, kept at 512 KB
    put(f"{nds}/Game G.nds", rom("GAME G", "GGGE")); put(f"{saves}/Game G.sav", b"\xff" * (512 * k))
    put(f"{roms}/Game G.nds", rom("GAME G", "GGGE")); put(f"{roms}/Game G.sav", save(17, 32 * k))
    # H: only in Maelle
    put(f"{roms}/Game H.nds", rom("GAME H", "HHHE")); put(f"{roms}/Game H.sav", save(19, 8 * k))

    # A FAT image of the DSi's card for the emulator; macOS only
    img = os.path.join(out, "sd.img")
    if shutil.which("hdiutil") and shutil.which("newfs_msdos"):
        if os.path.exists(img):
            os.remove(img)
        with open(img, "wb") as f:
            f.truncate(64 * 1024 * 1024)
        dev = subprocess.check_output(["hdiutil", "attach", "-nomount", "-imagekey",
                                       "diskimage-class=CRawDiskImage", img]).decode().split()[0]
        try:
            subprocess.check_call(["newfs_msdos", "-F", "32", "-v", "DSISD", dev], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            subprocess.check_call(["hdiutil", "detach", dev], stdout=subprocess.DEVNULL)
            mount = subprocess.check_output(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage",
                                             img]).decode().split("\t")[-1].strip()
            for root, _, files in os.walk(dsi):
                for name in files:
                    rel = os.path.relpath(os.path.join(root, name), dsi)
                    os.makedirs(os.path.join(mount, os.path.dirname(rel)), exist_ok=True)
                    shutil.copyfile(os.path.join(root, name), os.path.join(mount, rel))
            subprocess.call(["dot_clean", "-m", mount])
            subprocess.call(["find", mount, "-name", "._*", "-delete"])
            dev = mount
        finally:
            subprocess.call(["hdiutil", "detach", dev], stdout=subprocess.DEVNULL)
    print("made", out)


if __name__ == "__main__":
    main()
