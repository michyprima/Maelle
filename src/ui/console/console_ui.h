/*
    Copyright 2019-2026 Hydr8gon
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
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "screen_frame.h"
#include "menu_font.h"
#include "../screen_layout.h"
#include "../../core/core.h"
#include "../../core/defines.h"

enum MenuInputs {
    INPUT_A,
    INPUT_B,
    INPUT_SELECT,
    INPUT_START,
    INPUT_RIGHT,
    INPUT_LEFT,
    INPUT_UP,
    INPUT_DOWN,
    INPUT_R,
    INPUT_L,
    INPUT_X,
    INPUT_Y,
    INPUT_MENU,
    INPUT_FAST_HOLD,
    INPUT_FAST_TOGG,
    INPUT_SCRN_SWAP,
    INPUT_MIC_BLOW,
    INPUT_POINTER_TOUCH,
    INPUT_SUN_UP,
    INPUT_SUN_DOWN,
    INPUT_GUITAR_GREEN,
    INPUT_GUITAR_RED,
    INPUT_GUITAR_YELLOW,
    INPUT_GUITAR_BLUE,
    INPUT_PIANO_C, // Followed by the other 12 keys up to high C
    INPUT_PIANO_HIGH_C = INPUT_PIANO_C + 12,
    INPUT_MAX
};

struct MenuTouch {
    bool pressed;
    float x, y;

    MenuTouch(bool pressed, float x, float y):
        pressed(pressed), x(x), y(y) {}
};

struct MenuItem {
    std::string name;
    std::string setting;
    void *iconTex;
    uint8_t iconSize;
    bool header;
    bool separator = true; // Whether a line is drawn under it
    std::vector<std::string> tags; // Pills on a second line under the name, if any
    std::string file; // The file name behind a file browser item, which the name may not be

    MenuItem(std::string name, std::string setting = "", void *iconTex = nullptr, uint8_t iconSize = 0):
        name(name), setting(setting), iconTex(iconTex), iconSize(iconSize), header(false) {}
    MenuItem(std::string name, const char *setting): MenuItem(name, std::string(setting)) {} // Not the header one
    MenuItem(std::string name, bool header):
        name(name), setting(""), iconTex(nullptr), iconSize(0), header(header) {}
    bool operator<(const MenuItem &item) { return (name < item.name); }
};

class ConsoleUI {
public:
    static Core *core;
    static bool running;
    alignas(4096) static uint32_t framebuffer[256 * 192 * 8];
    static ScreenLayout layout;
    static bool gbaMode;
    static bool screenShift; // Whether the screen textures are upscaled

    // With high-res 3D, the GPU draws the 3D over native screens, so the CPU doesn't have to read the 3D output
    static uint32_t *hiResData; // The high-res 3D frame taken from the core
    static void *hiResTexture; // A texture of that frame, or a blank one when there isn't one
    static void *blankHiRes; // A blank texture for screens without high-res 3D, which shows their native 3D
    static bool hiResTop; // Whether the 3D is on the top screen

    // When the GPU builds the screens, it also converts the core's packed frames to colors, so the CPU doesn't have to
    static uint32_t *packedData; // The packed frame taken from the core, which the screen textures use, or null
    static uint32_t *gbaPackedData; // The same for the cropped GBA screen, whose frame has 15-bit colors

    // The GPU builds the screens at twice the size once for each new frame into textures of their own, keeping the last
    // ones, and the UI draws those, averaged with the last ones for ghosting
    static void *composed[2], *composedLast[2];
    static bool screensComposed; // Whether the screens being drawn are those
    static bool composedHigh; // Whether the last screens built are at twice the size, or else at the DS's
    static void composeScreens(void *top, void *bottom, void *hiRes, bool hiResTop, bool packed, bool high);
    static void composeGbaScreen(void *gba, bool packed, bool high);

    // With ghosting at native size, frames alternate between two halves of the framebuffer, so the last one is kept as
    // it was, and the GPU averages the two
    static uint32_t *screenFrame; // Where the current frame is
    static uint32_t *lastFrame; // Where the last frame is, or null without ghosting
    static void *drawLastScreen; // The last frame's screen to average with the next one, if any

    static int showFpsCounter;
    static int selectLastRom;
    static int menuTheme;
    static int frameColor; // The screen frame's color, or 0 for none
    static int gameLanguage; // The language games see, or 0 for the system's
    static std::string nickname; // The name games see in UTF-8, or empty for the Mii's
    static std::string syncId; // This Wii U's ID for save sync with a DSi, made the first time it's used
    static int micInput; // Whether games hear the GamePad's microphone
    static int stickPointer; // Whether the right stick moves a pointer on the touch screen, and when it hides
    static int keyBinds[INPUT_MAX];
    static int keyCombos[INPUT_SUN_DOWN - INPUT_MENU + 1]; // Two buttons held together for each hotkey, if any
    static int keyLayout; // The default layout the bindings were last moved to

    // Data that is defined per-platform
    static uint32_t defaultKeys[INPUT_MAX];
    static const char *keyNames[32];
    static const int micRate; // Microphone samples per second
    static const uint32_t pointerKeys; // The right stick's directions, which the pointer takes from the game

    // Functions that are implemented per-platform
    static void startFrame(uint32_t color, bool framed = false);
    static void endFrame();
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    static void gpuLap(int slot);
#endif
    static void *createTexture(uint32_t *data, int width, int height);
    static void writeTexture(void *texture, const uint32_t *data, int x, int y, int width, int height);
    static void destroyTexture(void *texture);
    static void releaseTexture(void *texture);
    static void drawFrames(void *tv, void *gamepad);
    static void *createHiResTexture(uint32_t *data);
    static void *createPackedTexture(uint32_t *data, int width, int height);
    static void destroyHiResTexture(void *texture);
    static void drawTexture(void *texture, float tx, float ty, float tw, float th, float x, float y,
        float w, float h, bool filter = true, int rotation = 0, uint32_t color = 0xFFFFFFFF);
    static void drawOverlay(void *texture, float x, float y, float w, float h, bool gamepad);
    static uint32_t getInputHeld();
    static MenuTouch getInputTouch();
    static int getGamepadBattery(); // -1 if it isn't connected
    static void setRumble(bool on);
    static void getInputStick(float &x, float &y);
    static size_t getMicSamples(int16_t *out, size_t max);
    static void closeMic();
    static std::u16string getMiiName(); // Empty if it can't be read
    static bool editText(std::u16string &text, int maxLength, int language, bool numpad = false); // False if canceled

    static void updateLights();
    static void drawRectangle(float x, float y, float w, float h, uint32_t color = 0xFFFFFFFF);
    static void drawString(std::string string, float x, float y,
        float size, uint32_t color = 0xFFFFFFFF, bool alignRight = false);
    static void fillAudioBuffer(uint32_t *buffer, int count, int rate);
    static void displayTick();
    static void relayout() { changed = true; }
#ifdef DRC_DEBUG
    static std::string displayDebugText();
#endif
    static uint32_t getInputPress();
    static uint32_t readInputEarly();
    static void earlyInput();
    static std::atomic<uint32_t> keysTaken; // Buttons the UI takes from the game, for the pointer or slot 2 accessories
    static uint32_t slot2Taken;

    static void initialize(int width, int height, std::string root, std::string prefix);
    static void mainLoop(MenuTouch (*specialTouch)() = nullptr, ScreenLayout *touchLayout = nullptr);
    static int setPath(std::string path);
    static bool fileBrowser();
    static void startCore();
    static void stopCore();
    static void closeCore();
    static void stopNetwork();
    static void stopSync();

private:
    static void *fileTextures[2];
    static void *folderTextures[2];
    static void *fontTexture;
    static void *fontAtlas; // The system font's letters, drawn as they're needed
    static void *circleTexture; // A white circle, whose halves are the ends of pills
    static MenuFont menuFont;
    static void *pointerTexture;

    static const uint32_t *palette;
    static uint32_t uiWidth, uiHeight;
    static uint32_t menuHeight, menuTop;
    static uint32_t lineHeight;
    static bool touchMode;

    static std::string ndsPath, gbaPath;
    static std::string basePath, curPath, defaultFolder, lastRom;
    static bool changed;
    static void *frameTextures[2]; // The screen frame for the TV and the gamepad
    static ScreenFrame::LightPatch lightPatches[2][ScreenFrame::LIGHT_COUNT]; // Each display frame's lights, lit and unlit
    static int lightsShown[2][3]; // The light lit in each of a frame's places for them, or -1 if it's unlit
    static void updateFrames(ScreenLayout *gamepad);

    // The right stick's pointer, in touch screen pixels, and when it hides
    static float pointerX, pointerY;
    static std::chrono::steady_clock::time_point pointerHide;
    static bool updatePointer(uint32_t held, ScreenLayout *gamepad);
    static void updateSlot2(uint32_t &pressed, uint32_t &held);

    static std::thread *coreThread;
    static WorkerThread *saveThread;
    static std::condition_variable cond;
    static std::mutex mutex;
    static int fpsLimiterBackup;

    static const uint32_t themeColors[];
    static const uint8_t charWidths[];

    ConsoleUI() {} // Private to prevent instantiation
    static void *bmpToTexture(uint8_t *bmp);
    static int stringWidth(const std::string &string);
    static std::string fitString(std::string string, int width);
    static void bannerTitle(const std::u16string &banner, std::string &name, std::vector<std::string> &tags);
    static void drawPill(float x, float y, float w, float h, uint32_t color);

    static uint32_t menu(std::string title, std::vector<MenuItem> &items,
        int &index, std::string actionX = "", std::string actionPlus = "", bool actionOk = true,
        std::string actionY = "", uint32_t hiddenKeys = 0, const std::vector<std::string> *tabs = nullptr,
        int *tab = nullptr);
    static uint32_t message(std::string title, std::string text, int type = 0);

#ifdef JIT_SELFTEST
    static void jitSelfTest();
#endif
#ifdef ROM_TEST
    static void romTest();
#endif
    static bool isHotkey(int input) { return input >= INPUT_MENU && input <= INPUT_SUN_DOWN; }
    static bool hotkeyHeld(int input, uint32_t held);
    static bool hotkeyPressed(int input, uint32_t pressed, uint32_t held);
    static void settingsMenu();
    static void aboutMenu();
    static void syncMenu();
    static void pauseMenu();
    static const int LAUNCH_FIRMWARE = -2;
    static int launchOptions(bool gba, bool rom);
    static int saveTypeMenu(bool gba);

    static bool createCore();
    static void runCore();

    // When syncing to the display, the emulator runs one frame for each display refresh, which the platform reports
    static std::mutex displayMutex;
    static std::condition_variable displayCond;
    static int displayTicks;
    static void waitForDisplay();
    static void checkSave();
};
