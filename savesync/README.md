# Maelle Sync

Syncs game saves between a real DSi and Maelle on the Wii U over the home network. The DSi runs the Maelle Sync app
(`dsi/`). Maelle hosts the server, under Settings > Maelle > Sync Saves with DSi, and `host/` has a Mac stand-in that
serves a folder of ROMs the way Maelle finds their saves.

- `common/`: shared by both sides, and built into Maelle. `sync_core` names games, hashes saves and decides which way
  each one goes, `sync_server` is the server, and `sync_net.h` reads and sends the protocol's lines. The protocol is
  described in `sync_server.h`. The server's sockets don't block and it waits with short sleeps, since the Wii U's
  `poll()` and `select()` fail with sockets numbered above 31.
- `dsi/`: the DSi app, built with devkitPro's libnds and calico, which drive the DSi's own Wi-Fi with WPA2. It needs
  DSi mode.
- `host/`: the Mac stand-in, the core's tests, the protocol tests and a test setup maker.
- `probe/`: the first network check: joins the network, finds a server and times a 256 KB echo.

## In Maelle

`src/ui/console/save_sync_ui.cpp` is the sync screen. Opening it closes a running game first, so its save is written
and nothing holds it. While the screen is open, a thread on core 2 connects the Wii U to its network, shows its address
and serves the saves of every `.nds` on the SD card (outside `wiiu/`), at the path Cartridge::setRom gives them,
taking the file lock for each read and write. The DSi finds it by broadcast on the same subnet; otherwise the address
shown is typed on the DSi, which remembers it. The Wii U's server ID is made once and kept in `maelle.ini` as `syncId`.

## How a sync works

1. The app finds `.nds`/`.dsi` files on the SD card and their saves as nds-bootstrap keeps them: in a `saves` folder
   next to the ROM, or beside it. It joins the network meanwhile, using System Settings' connections.
2. It finds Maelle with a UDP broadcast on port 5821. If none answers, the address can be typed in.
3. Games are matched by game code and header CRC, or by game code alone when each side has only one version.
4. Each save's content is hashed, without the 0xFF padding at its end, so the same save at two chip sizes counts as
   the same. Blank saves count as no save and never overwrite one with data. The DSi keeps, for each server in
   `sd:/_nds/maelle-sync.txt`, the hash both sides had at the last sync. A save that only one side changed goes to the
   other side, and a save both sides changed has to be chosen by hand.
5. Before anything is sent, the list shows which way each save will go, and any of them can be changed. A received
   save is written at the size the receiver already had, if it fits, and the save it replaces is kept as `.sav.bak`.

## Building

    cd dsi && DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make   # maelle-sync.nds
    host/build.sh                                                                # maelle-sync-host, core-test

## Testing on the Mac

    host/core-test
    host/make_fixture.py <dir>                  # fake ROMs + saves for each case, and <dir>/sd.img for the DSi
    host/maelle-sync-host <dir>/maelle/roms &
    host/protocol_test.py
    tests/jit/game.sh dsi/maelle-sync.nds <system files> 450 --modes portable --wifi dsi realtime \
        sd=<copy of sd.img> ppm=shot.ppm

In the emulator, the review screen is up by about frame 400. `keys=500:4,560:0` chooses DSi->U for the conflict and
syncs. The virtual network answers broadcasts from 127.0.0.1, so the app swaps that for the gateway (10.0.2.2).

## Testing on a real DSi without the Wii U

The Mac stand-in plays Maelle. To leave the Mac's own files alone, point its saves at an empty folder:

    host/maelle-sync-host ~/Downloads --saves /path/to/empty/folder --name "Mac"

Copy `dsi/maelle-sync.nds` to the DSi's SD card and start it from TWiLight Menu++, in DSi mode, on the same network
as the Mac. Games found in both places are listed. With an empty saves folder on the Mac, each DSi save is offered as
DSi->U, so the DSi's saves don't change. To test the other way, choose U->DSi for a game whose save is backed up, or
change a save on the Mac and sync again.
