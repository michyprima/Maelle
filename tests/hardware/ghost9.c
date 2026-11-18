// Shows whether a frontend's ghosting works: the main screen shows VRAM A and B on alternate frames, so every frame
// differs from the last one. A square is red in A and blue in B, next to a square of their mix, and a white bar moves
// 4 pixels a frame, drawn into the bank that's hidden at the time so nothing tears
#include <nds.h>
#include "screen.h"

#define BANK_A ((volatile unsigned short*)0x06800000)
#define BANK_B ((volatile unsigned short*)0x06820000)
#define COLOR(r, g, b) (0x8000 | (r) | ((g) << 5) | ((b) << 10))

static void fill(volatile unsigned short *bank, int x0, int y0, int w, int h, unsigned short color) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            bank[y * 256 + x] = color;
}

static void line(int row, const char *s, unsigned short color) {
    textRow = row;
    textColumn = 0;
    text(s, color);
}

int main(void) {
    // VRAM A is shown as a framebuffer, and B joins it, both with the same text and squares but for the left one's color
    screenInit();
    *(volatile unsigned char*)0x04000241 = 0x80;
    for (int i = 0; i < 256 * 192; i++) BANK_B[i] = BLACK;
    for (int b = 0; b < 2; b++) {
        screenBase = b ? BANK_B : BANK_A;
        line(0, "MAELLE GHOSTING TEST", WHITE);
        line(2, "LEFT: RED AND BLUE, SWAPPED EVERY FRAME", WHITE);
        line(3, "RIGHT: THEIR MIX, FOR REFERENCE", WHITE);
        line(4, "GHOSTING ON: LEFT LOOKS LIKE RIGHT", GREEN);
        line(5, "GHOSTING OFF: LEFT FLICKERS", YELLOW);
        line(6, "BAR: GHOSTING SHOWS A FAINT COPY BEHIND", WHITE);
        fill(screenBase, 16, 64, 96, 64, b ? COLOR(0, 0, 20) : COLOR(20, 0, 0));
        fill(screenBase, 144, 64, 96, 64, COLOR(10, 0, 10));
    }

    // Draw the bar into the hidden bank, erasing where it was two frames ago, then show that bank from the next frame
    int bar[2] = { -1, -1 };
    for (unsigned frame = 0; pmMainLoop(); frame++) {
        int b = frame & 1;
        volatile unsigned short *bank = b ? BANK_B : BANK_A;
        if (bar[b] >= 0) fill(bank, bar[b], 144, 8, 40, BLACK);
        bar[b] = (frame * 4) % 248;
        fill(bank, bar[b], 144, 8, 40, WHITE);
        threadWaitForVBlank();
        *(volatile unsigned*)0x04000000 = 0x00020000 | (b << 18);
    }
    return 0;
}
