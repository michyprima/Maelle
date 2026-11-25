/*
    Copyright 2026 michyprima

    Save sync network probe: checks that a DSi can join the home network (WPA2 in DSi mode) with the saved Wi-Fi
    settings, find a sync server with a UDP broadcast, and exchange data with it over TCP.
*/

#include <nds.h>
#include <dswifi9.h>
#include <wfc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>

#define SYNC_PORT 5821
#define PROBE_BYTES (256 * 1024)

static const char *wpaName(int mode) {
    switch (mode) {
        case WfcWpaMode_WPA_PSK_TKIP: return "WPA TKIP";
        case WfcWpaMode_WPA2_PSK_TKIP: return "WPA2 TKIP";
        case WfcWpaMode_WPA_PSK_AES: return "WPA AES";
        case WfcWpaMode_WPA2_PSK_AES: return "WPA2 AES";
        default: return "open/WEP";
    }
}

static bool waitKeyOrQuit(void) {
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_A) return true;
        if (keysDown() & KEY_START) return false;
    }
    return false;
}

// Broadcast a discovery request and wait up to 2 seconds for a server to answer; returns 0 if none did
static unsigned long discover(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    ioctl(sock, FIONBIO, &yes);

    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(SYNC_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;

    unsigned long found = 0;
    for (int frame = 0; frame < 120 && !found; frame++) {
        if (frame % 30 == 0)
            sendto(sock, "MAELLE?", 7, 0, (struct sockaddr*)&to, sizeof(to));
        char buf[16];
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &len);
        if (n >= 7 && !memcmp(buf, "MAELLE!", 7))
            found = from.sin_addr.s_addr;
        swiWaitForVBlank();
    }
    closesocket(sock);
    return found;
}

static bool sendAll(int sock, const void *data, int size) {
    const char *p = data;
    while (size > 0) {
        int n = send(sock, p, size, 0);
        if (n <= 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

static bool recvAll(int sock, void *data, int size) {
    char *p = data;
    while (size > 0) {
        int n = recv(sock, p, size, 0);
        if (n <= 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

// Say hello, then send a block of patterned data, have it echoed back, check it and time both ways
static void talk(unsigned long addr) {
    struct in_addr a = { addr };
    iprintf("TCP %s:%d ... ", inet_ntoa(a), SYNC_PORT);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(SYNC_PORT);
    to.sin_addr.s_addr = addr;
    if (sock < 0 || connect(sock, (struct sockaddr*)&to, sizeof(to)) < 0) {
        iprintf("failed\n");
        if (sock >= 0) closesocket(sock);
        return;
    }
    iprintf("ok\n");

    static u8 out[4096], in[4096];
    char hello[32];
    int len = snprintf(hello, sizeof(hello), "HELLO %s\n", isDSiMode() ? "DSi" : "DS");
    u32 size = PROBE_BYTES;
    bool ok = sendAll(sock, hello, len) && sendAll(sock, &size, 4);

    // Upload, then download the echo; the server sends it back only after it has the whole block
    cpuStartTiming(0);
    for (u32 done = 0; ok && done < size; done += sizeof(out)) {
        for (int i = 0; i < (int)sizeof(out); i++) out[i] = (u8)((done + i) * 7 + ((done + i) >> 8));
        ok = sendAll(sock, out, sizeof(out));
    }
    u32 upTicks = cpuGetTiming();
    u32 bad = 0;
    for (u32 done = 0; ok && done < size; done += sizeof(in)) {
        ok = recvAll(sock, in, sizeof(in));
        for (int i = 0; ok && i < (int)sizeof(in); i++)
            if (in[i] != (u8)((done + i) * 7 + ((done + i) >> 8))) bad++;
    }
    u32 totalTicks = cpuEndTiming();
    closesocket(sock);

    if (!ok) {
        iprintf("transfer broke off\n");
        return;
    }
    u32 upMs = timerTicks2msec(upTicks), downMs = timerTicks2msec(totalTicks - upTicks);
    iprintf("%luKB up   %lu ms (%lu KB/s)\n", size / 1024, upMs, upMs ? size / upMs : 0);
    iprintf("%luKB down %lu ms (%lu KB/s)\n", size / 1024, downMs, downMs ? size / downMs : 0);
    iprintf("bad bytes: %lu %s\n", bad, bad ? "FAIL" : "PASS");
}

int main(void) {
    consoleDemoInit();
    iprintf("Maelle save sync probe\n");
    iprintf("Mode: %s\n\n", isDSiMode() ? "DSi (DSi Wi-Fi chip)" : "DS (DS Wi-Fi chip)");
    iprintf("Connecting with saved settings\n");

    if (!Wifi_InitDefault(WFC_CONNECT)) {
        iprintf("Failed to connect!\n");
        iprintf("Check the connections in\nSystem Settings > Internet.\n");
        waitKeyOrQuit();
        return 0;
    }

    struct in_addr gateway, mask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
    WfcConnSlot *slot = wfcGetActiveSlot();
    if (slot) {
        char ssid[33] = {0};
        memcpy(ssid, slot->ssid, slot->ssid_len < 32 ? slot->ssid_len : 32);
        iprintf("SSID: %s\n", ssid);
        iprintf("Security: %s\n", slot->conn_type == WfcConnType_WpaNormal ?
            wpaName(((WfcConnSlotEx*)slot)->wpa_mode) : "open/WEP");
    }
    iprintf("IP: %s\n", inet_ntoa(ip));
    iprintf("Gateway: %s\n\n", inet_ntoa(gateway));

    do {
        iprintf("Looking for a server ... ");
        unsigned long server = discover();
        if (server && (ntohl(server) >> 24) == 127) {
            // The emulator's virtual network answers from the host's loopback, which is the gateway from in here
            iprintf("found (host)\n");
            server = gateway.s_addr;
        } else if (server) {
            iprintf("found\n");
        } else {
            // The emulator's virtual network puts the host at the gateway address, and doesn't pass broadcasts
            iprintf("none\nTrying the gateway\n");
            server = gateway.s_addr;
        }
        talk(server);
        iprintf("\nA: again  START: quit\n");
    } while (waitKeyOrQuit());
    return 0;
}
