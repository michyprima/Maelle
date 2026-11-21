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


#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/foreground.h>
#include <coreinit/internal.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#ifdef THREAD_PROBE
#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <vector>
#include <nn/ac.h>
#include <nsysnet/_socket.h>
#endif
#include <sys/stat.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2r/draw.h>
#include <mic/mic.h>
#include <nn/act.h>
#include <nn/swkbd.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <camera/camera.h>
#include <vpad/input.h>
#include <whb/sdcard.h>
#include <whb/gfx.h>
#include <SDL2/SDL.h>
#ifdef AUDIO_CAPTURE
#include <sndcore2/device.h>
#endif
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "console_ui.h"
#include "gpu3d_wiiu.h"
#include "../core/settings.h"
#include "../core/worker_thread.h"
#include "../core/heartbeat.h"
#include "../core/emu_profile.h"
#include "frame_trace.h"
#include "frame_log.h"
#include "../core/file_lock.h"

#define MAX_DRAWS 4096 // Quads the UI can draw in a frame, which long message boxes need a glyph each of
#define NO_INVALIDATE GX2RResourceFlags(GX2R_RESOURCE_DISABLE_CPU_INVALIDATE | GX2R_RESOURCE_DISABLE_GPU_INVALIDATE)
extern const uint8_t shader_wiiu_gsh[];
extern const uint8_t screen_wiiu_gsh[];
extern const uint8_t screen_packed_wiiu_gsh[];
extern const uint8_t screen_gba_wiiu_gsh[];
extern const uint8_t screen_blend_wiiu_gsh[];
extern const uint8_t migoomba_maelle_gba[], migoomba_maelle_gba_end[];

ScreenLayout gpLayout;
GX2Texture *gpTexture, *gpLastTexture;
int tvWidth, tvHeight;
int bufOffset;
bool firstScreen;

void updateCamera();
void closeCamera();

VPADStatus vpad;
VPADTouchData touch;
int gamepadBattery = -1; // The GamePad's battery byte from its last read, or -1 while it's not connected
uint32_t extHeld[4];
float extStick[4][2]; // Right stick positions of the other controllers
KPADStatus kpad[4]; // The other controllers' last reads, for the system keyboard
bool kpadRead[4]; // Whether each controller was read this frame
bool scanned;

WHBGfxShaderGroup group;
WHBGfxShaderGroup screenGroup; // Builds high-res screens, from native ones with high-res 3D over them
WHBGfxShaderGroup packedGroup; // The same from the core's packed native frames, converting their colors too
WHBGfxShaderGroup gbaGroup; // Builds the GBA screen from the core's 15-bit frames, converting their colors
WHBGfxShaderGroup blendGroup; // Draws native screens averaged with the last frame's, for ghosting
GX2RBuffer posBuffer, texBuffer, colBuffer;
GX2Sampler samplers[2];

// State shared between the main thread, which handles system messages, and the UI thread
// When the system asks for the foreground, the UI thread stops between frames until it gets it back
std::mutex procMutex;
std::condition_variable procCond;
std::atomic<bool> releaseRequested;
bool uiParked, uiDone;

// Exiting from the menus shuts down what the UI started, so the system doesn't wait on it when powering off later:
// background threads stop and the controllers and audio close while the app has the foreground, and graphics and
// the SD card once the system has taken it to exit
std::atomic<bool> appClosing;
bool exitRequested, uiClosed;
OSThread *vsyncThread, *hangThread;

// Whether the UI holds GX2, from starting a frame until it's finished; menus can start a frame and return without
// finishing it, so it's only locked once, since the mutex counts recursive locks
bool uiHoldsGx2;

// Frames the UI has finished, which the hang watchdog checks for progress
volatile uint32_t uiFrames;

// WHB sets up the GamePad's display buffer for the GamePad's display mode at startup, which is disabled if it's off
// then, so it gets no buffer and stays black after connecting; this sets one up when the mode changes, and again after
// getting the foreground back, since WHB sets its own up again then
GX2DrcRenderMode drcStartMode, drcBufferMode;
void *drcBuffer = nullptr;
uint32_t drcBufferSize = 0;

void updateDrcBuffer() {
    GX2DrcRenderMode mode = GX2GetSystemDRCMode();
    if (mode == GX2_DRC_RENDER_MODE_DISABLED || mode == drcBufferMode) return;
    uint32_t size, unknown;
    GX2CalcDRCSize(mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &size, &unknown);
    if (size > drcBufferSize) {
        MEMFreeToDefaultHeap(drcBuffer);
        if (!(drcBuffer = MEMAllocFromDefaultHeapEx(size, 0x1000))) {
            drcBufferSize = 0;
            return;
        }
        drcBufferSize = size;
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drcBuffer, size);
    GX2SetDRCBuffer(drcBuffer, size, mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
    GX2SetDRCScale(854, 480);
    drcBufferMode = mode;
}

#ifdef DRC_DEBUG
// GamePad display debugging: what GX2 and VPAD report about the GamePad, shown on the TV
std::atomic<uint32_t> drcAttaches(0), drcDetaches(0), drcDraws(0);
std::atomic<bool> drcAttached(false);
int lastVpadError = 0;

static void drcConnected(uint32_t, BOOL attached) {
    // Count GX2's reports of the GamePad connecting and disconnecting
    (attached ? drcAttaches : drcDetaches)++;
    drcAttached.store(attached);
}

std::string ConsoleUI::displayDebugText() {
    char text[512];
    snprintf(text, sizeof(text), "VPAD error %d, split %d, arrangement %d (setting %d) | DRC mode %d scan %d\n"
        "GX2 connects %u disconnects %u, attached %d | GamePad draws %u\n"
        "GamePad top %d,%d %dx%d bottom %d,%d %dx%d | display buffer mode %d", lastVpadError,
        ScreenLayout::splitDisplays, ScreenLayout::arrangement(), ScreenLayout::screenArrangement,
        (int)GX2GetSystemDRCMode(), (int)GX2GetSystemDRCScanMode(), drcAttaches.load(), drcDetaches.load(),
        (int)drcAttached.load(), drcDraws.load(), gpLayout.topX, gpLayout.topY, gpLayout.topWidth, gpLayout.topHeight,
        gpLayout.botX, gpLayout.botY, gpLayout.botWidth, gpLayout.botHeight, (int)drcBufferMode);
    return text;
}
#endif

uint32_t ConsoleUI::defaultKeys[] {
    VPAD_BUTTON_A, VPAD_BUTTON_B, VPAD_BUTTON_MINUS, VPAD_BUTTON_PLUS,
    VPAD_BUTTON_RIGHT | VPAD_STICK_R_EMULATION_RIGHT | VPAD_STICK_L_EMULATION_RIGHT,
    VPAD_BUTTON_LEFT | VPAD_STICK_R_EMULATION_LEFT | VPAD_STICK_L_EMULATION_LEFT,
    VPAD_BUTTON_UP | VPAD_STICK_R_EMULATION_UP | VPAD_STICK_L_EMULATION_UP,
    VPAD_BUTTON_DOWN | VPAD_STICK_R_EMULATION_DOWN | VPAD_STICK_L_EMULATION_DOWN,
    VPAD_BUTTON_R, VPAD_BUTTON_L, VPAD_BUTTON_X, VPAD_BUTTON_Y,
    VPAD_BUTTON_ZL | VPAD_BUTTON_ZR, 0, 0, 0, VPAD_BUTTON_STICK_L, VPAD_BUTTON_STICK_R,
    0, 0, // Sunlight
    VPAD_BUTTON_LEFT, VPAD_BUTTON_UP, VPAD_BUTTON_RIGHT, VPAD_BUTTON_DOWN, // Guitar Grip, round the d-pad
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 // Easy Piano
};

const uint32_t ConsoleUI::pointerKeys = VPAD_STICK_R_EMULATION_UP | VPAD_STICK_R_EMULATION_DOWN |
    VPAD_STICK_R_EMULATION_LEFT | VPAD_STICK_R_EMULATION_RIGHT;

const char *ConsoleUI::keyNames[] {
    "Sync", "Home", "Minus", "Plus", "R", "L", "ZR", "ZL",
    "Down", "Up", "Right", "Left", "Y", "X", "B", "A",
    "TV", "R Stick", "L Stick", "", "", "", "",
    "RS Down", "RS Up", "RS Right", "RS Left",
    "LS Down", "LS Up", "LS Right", "LS Left"
};

// The frames around the screens are only drawn again where something else was drawn over them, since the TV's and
// gamepad's color buffers keep what was drawn, the screens are opaque and drawn in the same places every frame, and
// drawing a whole 1080p frame takes the GPU over a millisecond; for each display, this keeps the frame texture drawn
// on it, and the areas to draw again, in display pixels, which overlays add for the next frame and lights for this one
struct FrameArea { int x0, y0, x1, y1; };
static void *frameShown[2];
static std::vector<FrameArea> frameRepaint[2], frameCovered[2];

static void addFrameArea(std::vector<FrameArea> &areas, FrameArea area) {
    // Add an area, merging it with one near it so a string's characters become one
    if (area.x0 >= area.x1 || area.y0 >= area.y1) return;
    for (FrameArea &other : areas) {
        if (area.x0 > other.x1 + 8 || other.x0 > area.x1 + 8 || area.y0 > other.y1 + 8 || other.y0 > area.y1 + 8)
            continue;
        other = { std::min(area.x0, other.x0), std::min(area.y0, other.y0), std::max(area.x1, other.x1),
            std::max(area.y1, other.y1) };
        return;
    }
    // Too many separate areas become one covering them all
    if (areas.size() >= 16) {
        for (const FrameArea &other : areas)
            area = { std::min(area.x0, other.x0), std::min(area.y0, other.y0), std::max(area.x1, other.x1),
                std::max(area.y1, other.y1) };
        areas.clear();
    }
    areas.push_back(area);
}

static void addFrameArea(std::vector<FrameArea> &areas, int display, float nx0, float ny0, float nx1, float ny1) {
    // Add an area given in the display's coordinates from -1 to 1, as whole pixels with a pixel to spare around it
    int width = display ? 854 : tvWidth, height = display ? 480 : tvHeight;
    addFrameArea(areas, { std::max(0, int((std::min(nx0, nx1) + 1) * width / 2) - 1),
        std::max(0, int((1 - std::max(ny0, ny1)) * height / 2) - 1),
        std::min(width, int((std::max(nx0, nx1) + 1) * width / 2) + 2),
        std::min(height, int((1 - std::min(ny0, ny1)) * height / 2) + 2) });
}

static void forgetFrames() {
    // Draw the frames whole next time, after the displays were drawn without them or their buffers were set up again
    for (int i = 0; i < 2; i++) {
        frameShown[i] = nullptr;
        frameRepaint[i].clear();
        frameCovered[i].clear();
    }
}

void releaseForeground() {
    // Stop the emulator, since threads on cores 0 and 1 are suspended in the background
    bool resume = ConsoleUI::running;
    ConsoleUI::stopCore();
    ConsoleUI::closeMic();
    closeCamera();

    // Let the main thread release the foreground, and wait until it's back
    std::unique_lock<std::mutex> lock(procMutex);
    uiParked = true;
    procCond.notify_all();
    procCond.wait(lock, [] { return !releaseRequested; });
    uiParked = false;
    drcBufferMode = drcStartMode;
    forgetFrames();
    lock.unlock();
    if (resume) ConsoleUI::startCore();
}

void ConsoleUI::startFrame(uint32_t color, bool framed) {
    // Stop drawing while the system needs the foreground, like for the HOME menu
    if (releaseRequested)
        releaseForeground();

    // Convert the clear color to floats
    float r = float(color & 0xFF) / 0xFF;
    float g = float((color >> 8) & 0xFF) / 0xFF;
    float b = float((color >> 16) & 0xFF) / 0xFF;
    float a = float(color >> 24) / 0xFF;

    // Clear the TV and gamepad screens, holding GX2 until the frame is submitted so the 3D renderer doesn't use it
    WHBGfxBeginRender();
    if (!uiHoldsGx2) {
        gx2Mutex.lock();
        uiHoldsGx2 = true;
        updateDrcBuffer();
    }
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    gpuLap(HB_UI_GPU_PRIOR_US);
#endif
    // Displays the game's frames will be drawn on are left as they are, since the frames are opaque and cover them
    // whole, and clearing a 1080p TV takes the GPU over a millisecond
    if (!framed) forgetFrames();
    WHBGfxBeginRenderTV();
    if (!framed || !frameTextures[0])
        WHBGfxClearColor(r, g, b, a);
    WHBGfxBeginRenderDRC();
    if (!framed || !frameTextures[1])
        WHBGfxClearColor(r, g, b, a);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    gpuLap(HB_UI_GPU_CLEAR_US);
#endif

}

#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
void ConsoleUI::gpuLap(int slot) {
    // Wait for the GPU to run what the UI added since the last lap, adding only the wait to the slot, so building the
    // commands on the CPU isn't counted; this serializes the UI's drawing with the 3D renderer, like its own timeline
    OSTime start = OSGetSystemTime();
    GX2DrawDone();
    Heartbeat::values[slot] += (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - start);
}
#endif

FILE *openHangReport(const char *reason) {
    // Append a report of a hang to hang.txt on the SD card, with every thread's state and stack, and return the file
    // so more can be added; it's formatted with the OS's functions, since stuck threads might hold C runtime locks,
    // and it's meant to be written from core 2, since file access from core 0 can stall
    static char path[256];
    __os_snprintf(path, sizeof(path), "%s/hang.txt", Settings::basePath.c_str());
    FILE *file = fopen(path, "a");
    if (!file) return nullptr;
    static char line[512];
    __os_snprintf(line, sizeof(line), "=== %s\ntime %llu, UI frames %u, GPU 3D stage %u frame %u, GX2 submitted %llu retired %llu\n",
        reason, (unsigned long long)OSGetTime(), uiFrames, gpu3dStage, gpu3dFrames,
        (unsigned long long)GX2GetLastSubmittedTimeStamp(), (unsigned long long)GX2GetRetiredTimeStamp());
    fputs(line, file);
    if (gpu3dDraw[0]) {
        fputs(gpu3dDraw, file);
        fputc('\n', file);
    }

    // Walk the OS's list of active threads; other cores keep running, so check tags and bound the walk
    OSThread *thread = OSGetCurrentThread();
    for (int i = 0; thread->activeLink.prev && i < 128; i++) thread = thread->activeLink.prev;
    for (int i = 0; thread && i < 128; i++, thread = thread->activeLink.next) {
        if (thread->tag != OS_THREAD_TAG) break;
        uint32_t sp = thread->context.gpr[1];
        int length = __os_snprintf(line, sizeof(line), "thread %08X %s state %u priority %d/%d affinity %u mutex %08X "
            "pc %08X lr %08X sp %08X stack:", (uint32_t)thread, thread->name ? thread->name : "?", thread->state,
            thread->priority, thread->basePriority, thread->attr & 7, (uint32_t)thread->mutex, thread->context.srr0,
            thread->context.lr, sp);
        for (int frame = 0; frame < 12 && sp >= 0x10000000 && sp < 0x50000000 && !(sp & 3); frame++) {
            uint32_t next = *(uint32_t*)sp;
            if (next <= sp || next >= 0x50000000 || (next & 3)) break;
            length += __os_snprintf(line + length, sizeof(line) - length, " %08X", *(uint32_t*)(next + 4));
            sp = next;
        }
        __os_snprintf(line + length, sizeof(line) - length, "\n");
        fputs(line, file);
    }
    return file;
}

int reportVsyncs(int, void *) {
    // Tell the UI each time the display refreshes, so the emulator can run one frame per refresh
    while (!appClosing.load()) {
        GX2WaitForVsync();
        ConsoleUI::displayTick();
    }
    return 0;
}

int watchHangs(int, void *) {
    // Write a hang report if the UI stops finishing frames for a few seconds, unless the system has it stopped; this
    // only wakes twice a second, and reports each hang once
    uint32_t last = 0;
    OSTime since = OSGetSystemTime();
    bool reported = false;
    while (!appClosing.load()) {
        OSSleepTicks(OSMillisecondsToTicks(500));
        OSTime now = OSGetSystemTime();
        if (uiFrames != last || uiParked || uiDone || releaseRequested.load()) {
            last = uiFrames;
            since = now;
            reported = false;
        }
        else if (!reported && OSTicksToMilliseconds(now - since) > 6000) {
            if (FILE *file = openHangReport("The UI stopped drawing for 6 seconds"))
                fclose(file);
            reported = true;
        }
    }
    return 0;
}

void ConsoleUI::endFrame() {
    // Finish and display a frame, doing what WHBGfxFinishRender does but waiting for the GPU without holding GX2,
    // so the 3D renderer can submit its frame in the meantime
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    gpuLap(HB_UI_GPU_REST_US);
#endif
    WHBGfxFinishRenderTV();
    for (int i = 0; i < 2; i++) {
        for (const FrameArea &area : frameCovered[i])
            addFrameArea(frameRepaint[i], area);
        frameCovered[i].clear();
    }
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    gpuLap(HB_UI_GPU_COPY_TV_US);
#endif
    WHBGfxFinishRenderDRC();
    GX2SwapScanBuffers();
    GX2Flush();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    gpuLap(HB_UI_GPU_COPY_DRC_US);
#endif
    OSTime submitted = GX2GetLastSubmittedTimeStamp();
    if (uiHoldsGx2) {
        uiHoldsGx2 = false;
        gx2Mutex.unlock();
    }
    GX2WaitTimeStamp(submitted);
    uiFrames = uiFrames + 1;

    // Free the gamepad screen texture if it was overridden
    if (gpTexture) {
        destroyTexture(gpTexture);
        gpTexture = nullptr;
    }
    if (gpLastTexture) {
        destroyTexture(gpLastTexture);
        gpLastTexture = nullptr;
    }

    // Reset the frame status
    bufOffset = 0;
    firstScreen = true;
    scanned = false;
}

// Texture images freed by the UI, kept for reuse since the screens are recreated every frame; allocating and freeing
// such large blocks all the time fragments the heap until they no longer fit
static std::mutex imagePoolMutex;
static GX2Surface imagePool[8];
static int imagePoolCount = 0;

static void *allocImage(GX2Surface &surface) {
    // Reuse a freed image of the same size if there is one
    {
        std::lock_guard<std::mutex> guard(imagePoolMutex);
        for (int i = 0; i < imagePoolCount; i++) {
            if (imagePool[i].imageSize != surface.imageSize || imagePool[i].alignment != surface.alignment) continue;
            void *image = imagePool[i].image;
            imagePool[i] = imagePool[--imagePoolCount];
            return image;
        }
    }

    // Allocate a new one, giving back the unused images first if the heap is out of room
    void *image = MEMAllocFromDefaultHeapEx(surface.imageSize, surface.alignment);
    if (!image) {
        std::lock_guard<std::mutex> guard(imagePoolMutex);
        while (imagePoolCount > 0)
            MEMFreeToDefaultHeap(imagePool[--imagePoolCount].image);
        image = MEMAllocFromDefaultHeapEx(surface.imageSize, surface.alignment);
    }
    return image;
}

static void freeImage(GX2Surface &surface) {
    // Keep the image for reuse, dropping the oldest one if the pool is full
    std::lock_guard<std::mutex> guard(imagePoolMutex);
    if (imagePoolCount == 8) {
        MEMFreeToDefaultHeap(imagePool[0].image);
        memmove(&imagePool[0], &imagePool[1], 7 * sizeof(GX2Surface));
        imagePoolCount--;
    }
    imagePool[imagePoolCount++] = surface;
}

void *ConsoleUI::createTexture(uint32_t *data, int width, int height) {
    // Create a new texture with the given dimensions
    GX2Texture *texture = new GX2Texture;
    memset(texture, 0, sizeof(GX2Texture));
    texture->surface.width = width;
    texture->surface.height = height;
    texture->surface.depth = 1;
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->viewNumSlices = 1;
    texture->compMap = 0x03020100;
    GX2CalcSurfaceSizeAndAlignment(&texture->surface);
    GX2InitTextureRegs(texture);
    // Point screen textures straight at the UI's framebuffer when the rows line up, so nothing has to be copied
    uint32_t *end = ConsoleUI::framebuffer + sizeof(ConsoleUI::framebuffer) / sizeof(uint32_t);
    if (data >= ConsoleUI::framebuffer && (uint8_t*)data + texture->surface.imageSize <= (uint8_t*)end &&
            texture->surface.pitch == uint32_t(width) && !((uintptr_t)data & (texture->surface.alignment - 1))) {
        texture->surface.image = data;
        if (!uiHoldsGx2) gx2Mutex.lock();
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
        if (!uiHoldsGx2) gx2Mutex.unlock();
        return texture;
    }

    texture->surface.image = allocImage(texture->surface);
    if (!texture->surface.image) {
        // Skip drawing it if there's no memory left, rather than crashing
        delete texture;
        return nullptr;
    }

    // Copy data to the texture
    uint32_t *dst = (uint32_t*)texture->surface.image;
    for (int y = 0; y < height; y++) {
        memcpy(dst, &data[y * width], width * sizeof(uint32_t));
        dst += texture->surface.pitch;
    }
    // Invalidating adds a GPU command, so hold GX2 for it if this is outside a frame
    if (!uiHoldsGx2) gx2Mutex.lock();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    if (!uiHoldsGx2) gx2Mutex.unlock();
    return texture;
}

void ConsoleUI::destroyTexture(void *texture) {
    // Clean up a texture
    if (!texture) return;
    for (int i = 0; i < 2; i++)
        if (texture == frameShown[i]) frameShown[i] = nullptr;
    uint32_t *image = (uint32_t*)((GX2Texture*)texture)->surface.image;
    if (image < ConsoleUI::framebuffer || image >= ConsoleUI::framebuffer + sizeof(ConsoleUI::framebuffer) / sizeof(uint32_t))
        freeImage(((GX2Texture*)texture)->surface);
    delete (GX2Texture*)texture;
}

void ConsoleUI::releaseTexture(void *texture) {
    // Clean up a large texture that won't be recreated, giving its memory back instead of keeping it for reuse
    if (!texture) return;
    for (int i = 0; i < 2; i++)
        if (texture == frameShown[i]) frameShown[i] = nullptr;
    MEMFreeToDefaultHeap(((GX2Texture*)texture)->surface.image);
    delete (GX2Texture*)texture;
}

static void drawQuad(GX2Texture *texture, bool filter, GX2Texture *last = nullptr) {
    // Draw a texture with the regular shader, or averaged with the last frame's for ghosting if given
    if (!last) {
        GX2SetPixelTexture(texture, group.pixelShader->samplerVars[0].location);
        GX2SetPixelSampler(&samplers[filter], group.pixelShader->samplerVars[0].location);
        GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
        return;
    }
    GX2SetFetchShader(&blendGroup.fetchShader);
    GX2SetVertexShader(blendGroup.vertexShader);
    GX2SetPixelShader(blendGroup.pixelShader);
    GX2SetPixelTexture(texture, blendGroup.pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&samplers[filter], blendGroup.pixelShader->samplerVars[0].location);
    GX2SetPixelTexture(last, blendGroup.pixelShader->samplerVars[1].location);
    GX2SetPixelSampler(&samplers[filter], blendGroup.pixelShader->samplerVars[1].location);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
}

// Targets for building the screens, at twice the size and at the DS's, two for each screen to keep the last ones for
// ghosting, with their own context state so the TV's isn't changed, and a quad covering them; the GBA screen has its
// own at its sizes, so filtering at its edges doesn't reach past them
GX2ContextState *composeState;
GX2ColorBuffer composeTargets[2][2], nativeTargets[2][2], gbaTargets[2][2];
GX2Texture composeTextures[2][2], nativeTextures[2][2], gbaTextures[2][2];
int composeCurrent = 0;
alignas(GX2_VERTEX_BUFFER_ALIGNMENT) float composePositions[] = { -1, 1, 1, 1, 1, -1, -1, -1 };
alignas(GX2_VERTEX_BUFFER_ALIGNMENT) float composeCoords[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
alignas(GX2_VERTEX_BUFFER_ALIGNMENT) float gbaCoords[] = { 0, 0, 0.9375f, 0, 0.9375f, 1, 0, 1 }; // 240 of 256 pixels
alignas(GX2_VERTEX_BUFFER_ALIGNMENT) float composeColors[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

static bool initTarget(GX2ColorBuffer &target, GX2Texture &texture, int width, int height) {
    // Create a target, tiled for the GPU, which the UI draws from as a texture
    memset(&target, 0, sizeof(target));
    target.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    target.surface.width = width;
    target.surface.height = height;
    target.surface.depth = 1;
    target.surface.mipLevels = 1;
    target.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    target.surface.aa = GX2_AA_MODE1X;
    target.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    target.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    target.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&target.surface);
    GX2InitColorBufferRegs(&target);
    if (!(target.surface.image = MEMAllocFromDefaultHeapEx(target.surface.imageSize, target.surface.alignment)))
        return false;
    memset(&texture, 0, sizeof(texture));
    texture.surface = target.surface;
    texture.surface.use = GX2_SURFACE_USE_TEXTURE;
    texture.viewNumSlices = 1;
    texture.compMap = 0x00010203;
    GX2InitTextureRegs(&texture);
    return true;
}

static bool initCompose() {
    // Create the targets for both sizes
    composeState = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!composeState) return false;
    GX2SetupContextStateEx(composeState, TRUE);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            if (!initTarget(composeTargets[i][j], composeTextures[i][j], 512, 384) ||
                    !initTarget(nativeTargets[i][j], nativeTextures[i][j], 256, 192) ||
                    !initTarget(gbaTargets[i][j], gbaTextures[i][j], i ? 480 : 240, i ? 320 : 160))
                return false;
        }
    }
    DCFlushRange(composePositions, sizeof(composePositions));
    DCFlushRange(composeCoords, sizeof(composeCoords));
    DCFlushRange(gbaCoords, sizeof(gbaCoords));
    DCFlushRange(composeColors, sizeof(composeColors));
    return true;
}

void ConsoleUI::composeScreens(void *top, void *bottom, void *hiRes, bool hiResTop, bool packed, bool high) {
    // Build both screens into the next targets, at twice the size if high, putting the high-res 3D over the screen it's
    // on, then set the TV's state up again for drawing; packed screens are the core's frames as they are, which the
    // shader converts
    WHBGfxShaderGroup &shaders = packed ? packedGroup : screenGroup;
    int width = high ? 512 : 256, height = high ? 384 : 192;
    composeCurrent ^= 1;
    composedHigh = high;
    GX2SetContextState(composeState);
    GX2SetViewport(0, 0, width, height, 0, 1);
    GX2SetScissor(0, 0, width, height);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
        (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
    GX2SetFetchShader(&shaders.fetchShader);
    GX2SetVertexShader(shaders.vertexShader);
    GX2SetPixelShader(shaders.pixelShader);
    GX2SetAttribBuffer(0, sizeof(composePositions), 2 * sizeof(float), composePositions);
    GX2SetAttribBuffer(1, sizeof(composeCoords), 2 * sizeof(float), composeCoords);
    GX2SetAttribBuffer(2, sizeof(composeColors), 4 * sizeof(float), composeColors);
    for (int i = 0; i < 2; i++) {
        GX2ColorBuffer &target = (high ? composeTargets : nativeTargets)[i][composeCurrent];
        GX2SetColorBuffer(&target, GX2_RENDER_TARGET_0);
        GX2SetPixelTexture((GX2Texture*)(i ? bottom : top), shaders.pixelShader->samplerVars[0].location);
        GX2SetPixelSampler(&samplers[0], shaders.pixelShader->samplerVars[0].location);
        GX2Texture *screenHiRes = (GX2Texture*)((hiResTop == !i && hiRes) ? hiRes : blankHiRes);
        GX2SetPixelTexture(screenHiRes, shaders.pixelShader->samplerVars[1].location);
        GX2SetPixelSampler(&samplers[0], shaders.pixelShader->samplerVars[1].location);
        GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
            target.surface.image, target.surface.imageSize);
        composedLast[i] = composed[i];
        composed[i] = &(high ? composeTextures : nativeTextures)[i][composeCurrent];
    }
    WHBGfxBeginRenderTV();
}

void ConsoleUI::composeGbaScreen(void *gba, bool packed, bool high) {
    // Build the GBA screen into the next target, at twice the size if high, then set the TV's state up again for
    // drawing; a packed screen is the core's frame as it is, 256 pixels to a line, which the shader converts
    WHBGfxShaderGroup &shaders = packed ? gbaGroup : group;
    int width = high ? 480 : 240, height = high ? 320 : 160;
    composeCurrent ^= 1;
    composedHigh = high;
    GX2SetContextState(composeState);
    GX2SetViewport(0, 0, width, height, 0, 1);
    GX2SetScissor(0, 0, width, height);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
        (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
    GX2SetFetchShader(&shaders.fetchShader);
    GX2SetVertexShader(shaders.vertexShader);
    GX2SetPixelShader(shaders.pixelShader);
    GX2SetAttribBuffer(0, sizeof(composePositions), 2 * sizeof(float), composePositions);
    GX2SetAttribBuffer(1, sizeof(gbaCoords), 2 * sizeof(float), packed ? gbaCoords : composeCoords);
    GX2SetAttribBuffer(2, sizeof(composeColors), 4 * sizeof(float), composeColors);
    GX2ColorBuffer &target = gbaTargets[high][composeCurrent];
    GX2SetColorBuffer(&target, GX2_RENDER_TARGET_0);
    GX2SetPixelTexture((GX2Texture*)gba, shaders.pixelShader->samplerVars[0].location);
    GX2SetPixelSampler(&samplers[0], shaders.pixelShader->samplerVars[0].location);
    GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
        target.surface.image, target.surface.imageSize);
    composedLast[0] = composed[0];
    composed[0] = &gbaTextures[high][composeCurrent];
    WHBGfxBeginRenderTV();
}

void *ConsoleUI::createHiResTexture(uint32_t *data) {
    // Wrap a high-res 3D frame from the core in a texture, using it in place; it has the 2D engine's packed 3D pixels,
    // 512 pixels to a line, which the screen shader unpacks
    if (!data) return blankHiRes;
    void *texture = createPackedTexture(data, 512, 384);
    return texture ? texture : blankHiRes;
}

void *ConsoleUI::createPackedTexture(uint32_t *data, int width, int height) {
    // Wrap a frame of the 2D engine's packed pixels from the core in a texture, using it in place, with the bytes of
    // each word in its channels from the top, or return null if its rows don't line up with a texture's; it's
    // destroyed like a high-res 3D texture
    GX2Texture *texture = new GX2Texture;
    memset(texture, 0, sizeof(GX2Texture));
    texture->surface.width = width;
    texture->surface.height = height;
    texture->surface.depth = 1;
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->viewNumSlices = 1;
    texture->compMap = 0x00010203;
    GX2CalcSurfaceSizeAndAlignment(&texture->surface);
    GX2InitTextureRegs(texture);
    if (texture->surface.pitch != uint32_t(width) || ((uintptr_t)data & (texture->surface.alignment - 1))) {
        delete texture;
        return nullptr;
    }
    texture->surface.image = data;

    // The GPU or the CPU might have written the frame, so flush it and drop any of it the texture cache has
    if (!uiHoldsGx2) gx2Mutex.lock();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture->surface.image, texture->surface.imageSize);
    if (!uiHoldsGx2) gx2Mutex.unlock();
    return texture;
}

#ifdef SHADER_TEST
static uint32_t testHash(uint32_t i) {
    // Mix a pixel's index into bits that look random, for the parts of words the shader should ignore
    i ^= i >> 16;
    i *= 0x7FEB352D;
    i ^= i >> 15;
    i *= 0x846CA68B;
    return i ^ (i >> 16);
}

static void shaderTest() {
    // Draw every 18-bit color through the packed screen shader, one per pixel, as a 2D pixel, as a 3D pixel with no
    // high-res 3D over it, and as high-res 3D over a 3D pixel, then every 15-bit color through the GBA screen shader,
    // eight times over, with the bits they should ignore set at random, and check the GPU's colors against the CPU's
    // conversion; the results go to shadertest.txt
    const int size = 512;
    std::string path = std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/maelle/shadertest.txt";
    FILE *file = fopen(path.c_str(), "w");
    if (!file) return;
    uint32_t *native = (uint32_t*)MEMAllocFromDefaultHeapEx(size * size * 4, 0x1000);
    uint32_t *hiRes = (uint32_t*)MEMAllocFromDefaultHeapEx(size * size * 4, 0x1000);
    GX2ColorBuffer target;
    memset(&target, 0, sizeof(target));
    target.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    target.surface.width = size;
    target.surface.height = size;
    target.surface.depth = 1;
    target.surface.mipLevels = 1;
    target.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    target.surface.aa = GX2_AA_MODE1X;
    target.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
    target.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    target.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&target.surface);
    GX2InitColorBufferRegs(&target);
    target.surface.image = MEMAllocFromDefaultHeapEx(target.surface.imageSize, target.surface.alignment);
    if (!native || !hiRes || !target.surface.image) {
        fprintf(file, "Not enough memory\n");
        fclose(file);
        return;
    }

    const char *names[] = { "2D pixels", "3D pixels without high-res 3D", "High-res 3D over 3D pixels", "GBA pixels" };
    uint32_t failed = 0;
    for (int pass = 0; pass < 4; pass++) {
        // The color under test is always the pixel's index; high-res alphas cycle through 1-63
        for (uint32_t i = 0; i < size * size; i++) {
            uint32_t junk = testHash(i), other = testHash(i + 0x12345);
            if (pass == 0) {
                native[i] = i | (junk & 0xF8FC0000) | (junk & 0x03000000);
                hiRes[i] = (other & ~0xFC0000) | ((1 + i % 63) << 18);
            }
            else if (pass == 1) {
                native[i] = i | BIT(26) | (junk & 0xFBFC0000);
                hiRes[i] = other & ~0xFC0000;
            }
            else if (pass == 2) {
                native[i] = (other & 0x3FFFF) | BIT(26) | (junk & 0xF8FC0000);
                hiRes[i] = i | ((1 + i % 63) << 18) | (junk & 0xFF000000);
            }
            else {
                native[i] = (i & 0x7FFF) | (junk & 0xFFFF8000);
                hiRes[i] = other;
            }
        }
        memset(target.surface.image, 0, target.surface.imageSize);
        DCFlushRange(target.surface.image, target.surface.imageSize);
        void *nativeTexture = ConsoleUI::createPackedTexture(native, size, size);
        void *hiResTexture = ConsoleUI::createPackedTexture(hiRes, size, size);
        if (!nativeTexture || !hiResTexture) {
            fprintf(file, "%s: couldn't make the textures\n", names[pass]);
            failed++;
            continue;
        }

        // Draw it like composeScreens does, or composeGbaScreen for GBA pixels though across the whole target, and
        // wait for the GPU
        WHBGfxShaderGroup &shaders = (pass == 3) ? gbaGroup : packedGroup;
        gx2Mutex.lock();
        GX2SetContextState(composeState);
        GX2SetViewport(0, 0, size, size, 0, 1);
        GX2SetScissor(0, 0, size, size);
        GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
        GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
        GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
            (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
        GX2SetFetchShader(&shaders.fetchShader);
        GX2SetVertexShader(shaders.vertexShader);
        GX2SetPixelShader(shaders.pixelShader);
        GX2SetAttribBuffer(0, sizeof(composePositions), 2 * sizeof(float), composePositions);
        GX2SetAttribBuffer(1, sizeof(composeCoords), 2 * sizeof(float), composeCoords);
        GX2SetAttribBuffer(2, sizeof(composeColors), 4 * sizeof(float), composeColors);
        GX2SetColorBuffer(&target, GX2_RENDER_TARGET_0);
        GX2SetPixelTexture((GX2Texture*)nativeTexture, shaders.pixelShader->samplerVars[0].location);
        GX2SetPixelSampler(&samplers[0], shaders.pixelShader->samplerVars[0].location);
        if (pass < 3) {
            GX2SetPixelTexture((GX2Texture*)hiResTexture, shaders.pixelShader->samplerVars[1].location);
            GX2SetPixelSampler(&samplers[0], shaders.pixelShader->samplerVars[1].location);
        }
        GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
        GX2DrawDone();
        gx2Mutex.unlock();
        DCInvalidateRange(target.surface.image, target.surface.imageSize);
        ConsoleUI::destroyHiResTexture(nativeTexture);
        ConsoleUI::destroyHiResTexture(hiResTexture);

        // Every pixel should be the CPU's 8-bit version of its color, opaque, with GBA colors doubled to 6 bits first
        uint32_t wrong = 0;
        for (uint32_t y = 0; y < size; y++) {
            for (uint32_t x = 0; x < size; x++) {
                uint32_t i = y * size + x;
                uint32_t got = ((uint32_t*)target.surface.image)[y * target.surface.pitch + x];
                uint32_t c = (pass == 3) ? (((i & 0x1F) << 1) | ((i & 0x3E0) << 2) | ((i & 0x7C00) << 3)) : i;
                uint32_t want = (((c >> 0) & 0x3F) * 255 / 63) << 24 | (((c >> 6) & 0x3F) * 255 / 63) << 16 |
                    (((c >> 12) & 0x3F) * 255 / 63) << 8 | 0xFF;
                if (got == want) continue;
                if (wrong++ < 10)
                    fprintf(file, "%s: x %u y %u, native %08X high-res %08X: got %08X, expected %08X\n", names[pass],
                        x, y, native[i], hiRes[i], got, want);
            }
        }
        fprintf(file, "%s: %u of %u pixels wrong\n", names[pass], wrong, size * size);
        if (wrong) failed++;
    }
    fprintf(file, failed ? "FAILED\n" : "PASSED\n");
    fclose(file);
    MEMFreeToDefaultHeap(native);
    MEMFreeToDefaultHeap(hiRes);
    MEMFreeToDefaultHeap(target.surface.image);
}
#endif

void ConsoleUI::destroyHiResTexture(void *texture) {
    // Clean up a high-res 3D texture, leaving its frame to the caller
    if (texture && texture != blankHiRes)
        delete (GX2Texture*)texture;
}

static void nextQuad() {
    // Move to the next quad's place in the buffers if there's one; past the last, further quads reuse it, which can
    // draw them wrong but stays inside the buffers
    if (size_t(bufOffset) < 8 * sizeof(float) * (MAX_DRAWS - 1))
        bufOffset += 8 * sizeof(float);
}

static void uploadQuad(const float *pos, const float *tex, const float *col) {
    // Upload positions to the buffer
    // Only the written range is flushed, since locking and unlocking normally flush the whole buffer
    uint8_t *buffer = (uint8_t*)GX2RLockBufferEx(&posBuffer, NO_INVALIDATE);
    memcpy(&buffer[bufOffset * 2], pos, posBuffer.elemSize * 8);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &buffer[bufOffset * 2], posBuffer.elemSize * 8);
    GX2RUnlockBufferEx(&posBuffer, NO_INVALIDATE);

    // Upload texture coordinates to the buffer
    buffer = (uint8_t*)GX2RLockBufferEx(&texBuffer, NO_INVALIDATE);
    memcpy(&buffer[bufOffset], tex, texBuffer.elemSize * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &buffer[bufOffset], texBuffer.elemSize * 4);
    GX2RUnlockBufferEx(&texBuffer, NO_INVALIDATE);

    // Upload vertex colors to the buffer
    buffer = (uint8_t*)GX2RLockBufferEx(&colBuffer, NO_INVALIDATE);
    memcpy(&buffer[bufOffset * 2], col, colBuffer.elemSize * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, &buffer[bufOffset * 2], colBuffer.elemSize * 4);
    GX2RUnlockBufferEx(&colBuffer, NO_INVALIDATE);
}

void ConsoleUI::drawTexture(void *texture, float tx, float ty, float tw, float th,
    float x, float y, float w, float h, bool filter, int rotation, uint32_t color) {
    // Skip textures that couldn't be created
    if (!texture) return;

    // Convert position values to floats for the TV
    float x1 = (x / (tvWidth / 2) - 1.0f);
    float y1 = -(y / (tvHeight / 2) - 1.0f);
    float x2 = ((x + w) / (tvWidth / 2) - 1.0f);
    float y2 = -((y + h) / (tvHeight / 2) - 1.0f);

    // Copy positions for the gamepad, but ensure lines are at least one pixel thick
    // A 4:3 TV shows the menus smaller and centered, so the gamepad maps them back to its full size, where touches are
    // Text over the game is placed on the whole TV, and keeps the same pixels on the gamepad
    float x1a = x1, y1a = y1, x2a = x2, y2a = y2;
    if (tvWidth * 9 / 16 < tvHeight) {
        float scale = 480.0f / menuHeight;
        x1a = x * scale / 427 - 1.0f;
        y1a = -((y - menuTop) * scale / 240 - 1.0f);
        x2a = (x + w) * scale / 427 - 1.0f;
        y2a = -((y + h - menuTop) * scale / 240 - 1.0f);
    }
    if (y1a - y2a < 1.0f / 240) y2a = y1a - 1.0f / 240;

    // Detect and override gamepad screen layout positions
    if (running && tw >= 240) {
        if (firstScreen) {
            x1a = (float(gpLayout.topX) / 427 - 1.0f);
            y1a = -(float(gpLayout.topY) / 240 - 1.0f);
            x2a = (float(gpLayout.topX + gpLayout.topWidth) / 427 - 1.0f);
            y2a = -(float(gpLayout.topY + gpLayout.topHeight) / 240 - 1.0f);
            firstScreen = false;
        }
        else {
            x1a = (float(gpLayout.botX) / 427 - 1.0f);
            y1a = -(float(gpLayout.botY) / 240 - 1.0f);
            x2a = (float(gpLayout.botX + gpLayout.botWidth) / 427 - 1.0f);
            y2a = -(float(gpLayout.botY + gpLayout.botHeight) / 240 - 1.0f);
        }
    }

    // Convert texture coordinates to floats
    float s1 = tx / ((GX2Texture*)texture)->surface.width;
    float t1 = ty / ((GX2Texture*)texture)->surface.height;
    float s2 = (tx + tw) / ((GX2Texture*)texture)->surface.width;
    float t2 = (ty + th) / ((GX2Texture*)texture)->surface.height;

    // Convert the surface color to floats
    float r = float((color >> 0) & 0xFF) / 0xFF;
    float g = float((color >> 8) & 0xFF) / 0xFF;
    float b = float((color >> 16) & 0xFF) / 0xFF;
    float a = float((color >> 24) & 0xFF) / 0xFF;

    // Define arrays for position and color values
    float posCoords[] = { x1, y1, x2, y1, x2, y2, x1, y2, x1a, y1a, x2a, y1a, x2a, y2a, x1a, y2a };
    float vtxColors[] = { r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a };

    // Define texture coordinates based on rotation
    float texCoords[] = {
        s1, t1, s2, t1, s2, t2, s1, t2, // None
        s1, t2, s1, t1, s2, t1, s2, t2, // Clockwise
        s2, t1, s2, t2, s1, t2, s1, t1, // Counter-clockwise
    };

    uploadQuad(posCoords, &texCoords[rotation * 8], vtxColors);
    if (!(running && tw >= 240)) {
        addFrameArea(frameCovered[0], 0, x1, y1, x2, y2);
        addFrameArea(frameCovered[1], 1, x1a, y1a, x2a, y2a);
    }
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    bool lapScreen = running && tw >= 240;
    if (lapScreen) gpuLap(HB_UI_GPU_REST_US);
#endif

    // Draw a texture on the TV, with high-res 3D over it if given
    WHBGfxBeginRenderTV();
    GX2RSetAttributeBuffer(&posBuffer, 0, posBuffer.elemSize, bufOffset * 2);
    GX2RSetAttributeBuffer(&texBuffer, 1, texBuffer.elemSize, bufOffset);
    GX2RSetAttributeBuffer(&colBuffer, 2, colBuffer.elemSize, bufOffset * 2);
    drawQuad((GX2Texture*)texture, filter, (GX2Texture*)drawLastScreen);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    if (lapScreen) gpuLap(HB_UI_GPU_SCREENS_TV_US);
#endif

    // Override the gamepad screen texture with the other screen in single screen mode
    GX2Texture *tempTexture = nullptr, *tempLast = (GX2Texture*)drawLastScreen;
    if (running && tw >= 240 && !(ConsoleUI::gbaMode && ScreenLayout::gbaCrop) && ScreenLayout::arrangement() == 3) {
        bool bottom = (ScreenLayout::screenSizing < 2);
        if (screensComposed) {
            tempTexture = (GX2Texture*)composed[bottom];
            if (drawLastScreen) tempLast = (GX2Texture*)composedLast[bottom];
        }
        else {
            bool shift = screenShift;
            uint32_t *data = &screenFrame[(256 * 192 * bottom) << (shift * 2)];
            if (gpTexture) destroyTexture(gpTexture);
            tempTexture = gpTexture = (GX2Texture*)createTexture(data, 256 << shift, 192 << shift);
            if (drawLastScreen && lastFrame) {
                if (gpLastTexture) destroyTexture(gpLastTexture);
                tempLast = gpLastTexture = (GX2Texture*)createTexture(&lastFrame[256 * 192 * bottom], 256, 192);
            }
        }
    }

    // Draw a texture on the gamepad
#ifdef DRC_DEBUG
    drcDraws++;
#endif
    WHBGfxBeginRenderDRC();
    GX2RSetAttributeBuffer(&posBuffer, 0, posBuffer.elemSize, bufOffset * 2 + 8 * sizeof(float));
    GX2RSetAttributeBuffer(&texBuffer, 1, texBuffer.elemSize, bufOffset);
    GX2RSetAttributeBuffer(&colBuffer, 2, colBuffer.elemSize, bufOffset * 2);
    drawQuad(tempTexture ? tempTexture : (GX2Texture*)texture, filter, tempLast);
#if defined(HEARTBEAT) && defined(GPU_TIMELINE)
    if (lapScreen) gpuLap(HB_UI_GPU_SCREENS_DRC_US);
#endif

    // Move to the next quad in the buffers
    nextQuad();
}

void ConsoleUI::writeTexture(void *texture, const uint32_t *data, int x, int y, int width, int height) {
    // Write part of a texture in place, for small changes that aren't worth a new texture
    if (!texture) return;
    GX2Surface &surface = ((GX2Texture*)texture)->surface;
    uint32_t *dst = (uint32_t*)surface.image + y * surface.pitch + x;
    for (int row = 0; row < height; row++)
        memcpy(dst + row * surface.pitch, &data[row * width], width * sizeof(uint32_t));
    // Invalidating adds a GPU command, so hold GX2 for it if this is outside a frame
    if (!uiHoldsGx2) gx2Mutex.lock();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, dst, ((height - 1) * surface.pitch + width) * sizeof(uint32_t));
    if (!uiHoldsGx2) gx2Mutex.unlock();

    // Draw the part again if it's a frame, like when a light changes
    for (int i = 0; i < 2; i++) {
        if (texture != frameShown[i]) continue;
        float sx = 2.0f / surface.width, sy = 2.0f / surface.height;
        addFrameArea(frameRepaint[i], i, x * sx - 1, 1 - y * sy, (x + width) * sx - 1, 1 - (y + height) * sy);
    }
}

void ConsoleUI::drawFrames(void *tv, void *gamepad) {
    // Fill each display with its own frame, without counting as a screen for the gamepad's layout; a frame that's
    // already there is only drawn again where it was covered or changed, limited to those areas by the scissor
    void *textures[] = { tv, gamepad };
    bool needed = false;
    for (int i = 0; i < 2; i++)
        needed |= textures[i] && (textures[i] != frameShown[i] || !frameRepaint[i].empty());
    if (!needed) return;
    static const float posCoords[] = { -1, 1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, 1, -1, -1, -1 };
    static const float texCoords[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    static const float vtxColors[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    uploadQuad(posCoords, texCoords, vtxColors);

    for (int i = 0; i < 2; i++) {
        if (!textures[i]) continue;
        if (i) WHBGfxBeginRenderDRC();
        else WHBGfxBeginRenderTV();
        GX2RSetAttributeBuffer(&posBuffer, 0, posBuffer.elemSize, bufOffset * 2 + (i ? 8 * sizeof(float) : 0));
        GX2RSetAttributeBuffer(&texBuffer, 1, texBuffer.elemSize, bufOffset);
        GX2RSetAttributeBuffer(&colBuffer, 2, colBuffer.elemSize, bufOffset * 2);
        if (textures[i] != frameShown[i]) {
            drawQuad((GX2Texture*)textures[i], false);
            frameShown[i] = textures[i];
        }
        else if (!frameRepaint[i].empty()) {
            for (const FrameArea &area : frameRepaint[i]) {
                GX2SetScissor(area.x0, area.y0, area.x1 - area.x0, area.y1 - area.y0);
                drawQuad((GX2Texture*)textures[i], false);
            }
            GX2SetScissor(0, 0, i ? 854 : tvWidth, i ? 480 : tvHeight);
        }
        frameRepaint[i].clear();
    }

    // Move to the next quad in the buffers
    nextQuad();
}

void ConsoleUI::drawOverlay(void *texture, float x, float y, float w, float h, bool gamepad) {
    // Draw a whole texture on one display, placed in that display's pixels
    if (!texture) return;
    float width = gamepad ? 854 : tvWidth, height = gamepad ? 480 : tvHeight;
    float x1 = x / (width / 2) - 1.0f, y1 = -(y / (height / 2) - 1.0f);
    float x2 = (x + w) / (width / 2) - 1.0f, y2 = -((y + h) / (height / 2) - 1.0f);
    float posCoords[] = { x1, y1, x2, y1, x2, y2, x1, y2, x1, y1, x2, y1, x2, y2, x1, y2 };
    static const float texCoords[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    static const float vtxColors[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    uploadQuad(posCoords, texCoords, vtxColors);
    addFrameArea(frameCovered[gamepad], gamepad, x1, y1, x2, y2);

    if (gamepad) WHBGfxBeginRenderDRC();
    else WHBGfxBeginRenderTV();
    GX2RSetAttributeBuffer(&posBuffer, 0, posBuffer.elemSize, bufOffset * 2 + (gamepad ? 8 * sizeof(float) : 0));
    GX2RSetAttributeBuffer(&texBuffer, 1, texBuffer.elemSize, bufOffset);
    GX2RSetAttributeBuffer(&colBuffer, 2, colBuffer.elemSize, bufOffset * 2);
    drawQuad((GX2Texture*)texture, true);

    // Move to the next quad in the buffers
    nextQuad();
}

uint32_t extToVpad(uint32_t hold, bool pro) {
    // Convert Pro Controller or Classic Controller buttons to GamePad buttons, so key binds apply to both
    // The face buttons, shoulders, d-pad, plus, minus and home use the same bits on both controllers
    static const uint32_t buttons[][2] = {
        { WPAD_PRO_BUTTON_A, VPAD_BUTTON_A }, { WPAD_PRO_BUTTON_B, VPAD_BUTTON_B },
        { WPAD_PRO_BUTTON_X, VPAD_BUTTON_X }, { WPAD_PRO_BUTTON_Y, VPAD_BUTTON_Y },
        { WPAD_PRO_BUTTON_L, VPAD_BUTTON_L }, { WPAD_PRO_BUTTON_R, VPAD_BUTTON_R },
        { WPAD_PRO_BUTTON_ZL, VPAD_BUTTON_ZL }, { WPAD_PRO_BUTTON_ZR, VPAD_BUTTON_ZR },
        { WPAD_PRO_BUTTON_PLUS, VPAD_BUTTON_PLUS }, { WPAD_PRO_BUTTON_MINUS, VPAD_BUTTON_MINUS },
        { WPAD_PRO_BUTTON_HOME, VPAD_BUTTON_HOME }, { WPAD_PRO_BUTTON_UP, VPAD_BUTTON_UP },
        { WPAD_PRO_BUTTON_DOWN, VPAD_BUTTON_DOWN }, { WPAD_PRO_BUTTON_LEFT, VPAD_BUTTON_LEFT },
        { WPAD_PRO_BUTTON_RIGHT, VPAD_BUTTON_RIGHT }
    };
    static const uint32_t proSticks[][2] = {
        { WPAD_PRO_BUTTON_STICK_L, VPAD_BUTTON_STICK_L }, { WPAD_PRO_BUTTON_STICK_R, VPAD_BUTTON_STICK_R },
        { WPAD_PRO_STICK_L_EMULATION_UP, VPAD_STICK_L_EMULATION_UP },
        { WPAD_PRO_STICK_L_EMULATION_DOWN, VPAD_STICK_L_EMULATION_DOWN },
        { WPAD_PRO_STICK_L_EMULATION_LEFT, VPAD_STICK_L_EMULATION_LEFT },
        { WPAD_PRO_STICK_L_EMULATION_RIGHT, VPAD_STICK_L_EMULATION_RIGHT },
        { WPAD_PRO_STICK_R_EMULATION_UP, VPAD_STICK_R_EMULATION_UP },
        { WPAD_PRO_STICK_R_EMULATION_DOWN, VPAD_STICK_R_EMULATION_DOWN },
        { WPAD_PRO_STICK_R_EMULATION_LEFT, VPAD_STICK_R_EMULATION_LEFT },
        { WPAD_PRO_STICK_R_EMULATION_RIGHT, VPAD_STICK_R_EMULATION_RIGHT }
    };
    static const uint32_t classicSticks[][2] = {
        { WPAD_CLASSIC_STICK_L_EMULATION_UP, VPAD_STICK_L_EMULATION_UP },
        { WPAD_CLASSIC_STICK_L_EMULATION_DOWN, VPAD_STICK_L_EMULATION_DOWN },
        { WPAD_CLASSIC_STICK_L_EMULATION_LEFT, VPAD_STICK_L_EMULATION_LEFT },
        { WPAD_CLASSIC_STICK_L_EMULATION_RIGHT, VPAD_STICK_L_EMULATION_RIGHT },
        { WPAD_CLASSIC_STICK_R_EMULATION_UP, VPAD_STICK_R_EMULATION_UP },
        { WPAD_CLASSIC_STICK_R_EMULATION_DOWN, VPAD_STICK_R_EMULATION_DOWN },
        { WPAD_CLASSIC_STICK_R_EMULATION_LEFT, VPAD_STICK_R_EMULATION_LEFT },
        { WPAD_CLASSIC_STICK_R_EMULATION_RIGHT, VPAD_STICK_R_EMULATION_RIGHT }
    };

    uint32_t result = 0;
    for (auto &b : buttons)
        if (hold & b[0]) result |= b[1];
    if (pro) {
        for (auto &b : proSticks)
            if (hold & b[0]) result |= b[1];
    }
    else {
        for (auto &b : classicSticks)
            if (hold & b[0]) result |= b[1];
    }
    return result;
}

// The controllers' last reads, made by the emulator thread just before each frame so games get presses sooner, or by the
// UI when there's no new one; the UI takes each into its own copies, so both use the same samples
struct Pads {
    VPADStatus vpad;
    VPADReadError vpadError;
    VPADTouchData touch;
    uint32_t extHeld[4];
    float extStick[4][2];
    KPADStatus kpad[4];
    bool kpadRead[4];
};
static Pads pads;
static bool padsFresh = false;
static std::mutex padsMutex;

static void readPads() {
    // Read the GamePad and the Pro Controllers and Classic Controllers on every channel
    // Channels without new data keep their last state, and disconnected ones are cleared
    // The emulator thread can read more often than the GamePad sends, like when fast-forwarding, so the GamePad keeps
    // its last state when there's no new sample, and is cleared when it's gone
    VPADStatus status;
    VPADRead(VPAD_CHAN_0, &status, 1, &pads.vpadError);
    if (pads.vpadError == VPAD_READ_SUCCESS)
        pads.vpad = status;
    else if (pads.vpadError != VPAD_READ_NO_SAMPLES)
        memset(&pads.vpad, 0, sizeof(pads.vpad));
    VPADGetTPCalibratedPoint(VPAD_CHAN_0, &pads.touch, &pads.vpad.tpNormal);
    for (int i = 0; i < 4; i++) {
        KPADStatus &status = pads.kpad[i];
        KPADError error;
        if (!(pads.kpadRead[i] = KPADReadEx((KPADChan)i, &status, 1, &error))) {
            if (error == KPAD_ERROR_INVALID_CONTROLLER) {
                pads.extHeld[i] = 0;
                pads.extStick[i][0] = pads.extStick[i][1] = 0;
            }
            continue;
        }
        if (status.extensionType == WPAD_EXT_PRO_CONTROLLER) {
            pads.extHeld[i] = extToVpad(status.pro.hold, true);
            pads.extStick[i][0] = status.pro.rightStick.x;
            pads.extStick[i][1] = status.pro.rightStick.y;
        }
        else if (status.extensionType == WPAD_EXT_CLASSIC || status.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
            pads.extHeld[i] = extToVpad(status.classic.hold, false);
            pads.extStick[i][0] = status.classic.rightStick.x;
            pads.extStick[i][1] = status.classic.rightStick.y;
        }
        else {
            pads.extHeld[i] = 0;
            pads.extStick[i][0] = pads.extStick[i][1] = 0;
        }
    }
}

uint32_t ConsoleUI::readInputEarly() {
    // Read the controllers for the emulator thread, returning the mappable keys held
    std::lock_guard<std::mutex> guard(padsMutex);
    readPads();
    padsFresh = true;
    uint32_t held = pads.vpad.hold;
    for (int i = 0; i < 4; i++)
        held |= pads.extHeld[i];
    return held & 0x7FFFFFFF;
}

void scanInput() {
    // Scan for input once per frame
    if (scanned) return;
    scanned = true;
    // Take the emulator thread's read if there's a new one, or read the controllers here
    VPADReadError vpadError;
    {
        std::lock_guard<std::mutex> guard(padsMutex);
        if (!padsFresh) readPads();
        padsFresh = false;
        vpad = pads.vpad;
        vpadError = pads.vpadError;
        touch = pads.touch;
        memcpy(extHeld, pads.extHeld, sizeof(extHeld));
        memcpy(extStick, pads.extStick, sizeof(extStick));
        memcpy(kpad, pads.kpad, sizeof(kpad));
        memcpy(kpadRead, pads.kpadRead, sizeof(kpadRead));
    }
#ifdef DRC_DEBUG
    lastVpadError = vpadError;
#endif

    // Give the GamePad the bottom screen with the TV + GamePad arrangement while it's connected, once it's been
    // connected or disconnected for half a second, so a brief signal loss doesn't move the screens around
    static int gamepadFrames = 0;
    bool gamepad = (vpadError != VPAD_READ_INVALID_CONTROLLER);
    if (!gamepad) gamepadBattery = -1;
    else if (vpadError == VPAD_READ_SUCCESS) gamepadBattery = vpad.battery;

    // Let the DSi see headphones plugged into the GamePad, and its camera when one of the DSi's is on
    if (vpadError == VPAD_READ_SUCCESS) DsiGpio::headphonesIn.store(vpad.usingHeadphones);
    updateCamera();
    if (gamepad == ScreenLayout::splitDisplays) {
        gamepadFrames = 0;
    }
    else if (++gamepadFrames >= 30) {
        ScreenLayout::splitDisplays = gamepad;
        ConsoleUI::relayout();
        gamepadFrames = 0;
    }
}

uint32_t ConsoleUI::getInputHeld() {
    // Return a mask of mappable keys from the GamePad and any other controllers
    scanInput();
    uint32_t held = vpad.hold;
    for (int i = 0; i < 4; i++)
        held |= extHeld[i];
    return held & 0x7FFFFFFF;
}

int ConsoleUI::getGamepadBattery() {
    // Return the GamePad's battery byte, or -1 if it isn't connected
    scanInput();
    return gamepadBattery;
}

void ConsoleUI::setRumble(bool on) {
    // Rumble the GamePad and any Pro Controllers, only telling them when it changes
    // The GamePad plays a pattern instead of staying on, so it gets it again every half second while rumbling continues
    static bool rumbling = false;
    static int frames = 0;
    if (on == rumbling && (!on || ++frames < 30)) return;
    rumbling = on;
    frames = 0;
    if (on) {
        static const uint8_t pattern[15] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        VPADControlMotor(VPAD_CHAN_0, pattern, 120);
    }
    else {
        VPADStopMotor(VPAD_CHAN_0);
    }
    for (int i = 0; i < 4; i++)
        WPADControlMotor((WPADChan)i, on);
}

MenuTouch ConsoleUI::getInputTouch() {
    // Return touch data from the GamePad
    scanInput();
    return MenuTouch(touch.touched, touch.x, touch.y);
}

void ConsoleUI::getInputStick(float &x, float &y) {
    // Return the right stick that's pushed furthest, from the GamePad or any other controller
    scanInput();
    x = vpad.rightStick.x;
    y = vpad.rightStick.y;
    for (int i = 0; i < 4; i++) {
        if (extStick[i][0] * extStick[i][0] + extStick[i][1] * extStick[i][1] > x * x + y * y) {
            x = extStick[i][0];
            y = extStick[i][1];
        }
    }
}

std::u16string ConsoleUI::getMiiName() {
    // Read the current Wii U user's Mii name, which is up to 10 characters like a DS nickname
    int16_t name[nn::act::MiiNameSize] = {};
    nn::act::Initialize();
    bool read = nn::act::GetMiiName(name).IsSuccess();
    nn::act::Finalize();
    std::u16string result;
    for (int i = 0; read && size_t(i) < nn::act::MiiNameSize && name[i]; i++)
        result += char16_t(name[i]);
    return result;
}

static void drawKeyboard(bool tv) {
    // Draw the system keyboard with its own GX2 state, since it leaves its state set and would break the UI's drawing
    static GX2ContextState *state = nullptr;
    if (!state) {
        state = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
        if (!state) return;
        GX2SetupContextStateEx(state, TRUE);
    }
    GX2ColorBuffer *color = tv ? WHBGfxGetTVColourBuffer() : WHBGfxGetDRCColourBuffer();
    GX2DepthBuffer *depth = tv ? WHBGfxGetTVDepthBuffer() : WHBGfxGetDRCDepthBuffer();
    GX2SetContextState(state);
    GX2SetColorBuffer(color, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(depth);
    GX2SetViewport(0, 0, color->surface.width, color->surface.height, 0, 1);
    GX2SetScissor(0, 0, color->surface.width, color->surface.height);
    if (tv) nn::swkbd::DrawTV();
    else nn::swkbd::DrawDRC();
}

bool ConsoleUI::editText(std::u16string &text, int maxLength, int language, bool numpad) {
    // Choose the keyboard's region from the console's, so it has the layouts the user is used to
    nn::swkbd::RegionType region = nn::swkbd::RegionType::Europe;
    int32_t mcp = MCP_Open();
    alignas(0x40) MCPSysProdSettings product = {};
    if (mcp >= 0 && MCP_GetSysProdSettings(mcp, &product) >= 0) {
        switch (product.product_area) {
            case MCP_REGION_JAPAN: region = nn::swkbd::RegionType::Japan; break;
            case MCP_REGION_USA: region = nn::swkbd::RegionType::USA; break;
            case MCP_REGION_CHINA: region = nn::swkbd::RegionType::China; break;
            case MCP_REGION_KOREA: region = nn::swkbd::RegionType::Korea; break;
            case MCP_REGION_TAIWAN: region = nn::swkbd::RegionType::Taiwan; break;
            default: break;
        }
    }
    if (mcp >= 0) MCP_Close(mcp);

    // Start the system keyboard, which only takes its memory while it's open
    FSClient *client = (FSClient*)MEMAllocFromDefaultHeap(sizeof(FSClient));
    void *work = MEMAllocFromDefaultHeap(nn::swkbd::GetWorkMemorySize(0));
    if (!client || !work) {
        if (client) MEMFreeToDefaultHeap(client);
        if (work) MEMFreeToDefaultHeap(work);
        return false;
    }
    // Hold the file lock while the keyboard is open, since it reads its resources through its own file client and
    // file access from several threads at once can stall the console
    std::lock_guard<FileLock> guard(fileLock());
    FSInit();
    FSAddClient(client, FS_ERROR_FLAG_NONE);
    nn::swkbd::CreateArg create;
    create.workMemory = work;
    create.regionType = region;
    create.fsClient = client;
    bool created = nn::swkbd::Create(create);

    // Show a text box with the current text, in the system's language
    bool decided = false;
    if (created) {
        nn::swkbd::AppearArg appear;
        appear.keyboardArg.configArg.languageType = nn::swkbd::LanguageType(language);
        appear.keyboardArg.configArg.disableNewLine = true;
        if (numpad) {
            // Use the number pad, with a dot key for addresses
            appear.keyboardArg.configArg.keyboardMode = nn::swkbd::KeyboardMode::Numpad;
            appear.keyboardArg.configArg.numpadCharLeft = u'.';
        }
        appear.inputFormArg.initialText = text.c_str();
        appear.inputFormArg.maxTextLength = maxLength;
        appear.inputFormArg.higlightInitialText = true;
        nn::swkbd::AppearInputForm(appear);

        // Run the keyboard until a choice is made, then until it finishes hiding, giving up on that after 2 seconds
        bool closing = false;
        int closingFrames = 0;
        while (!closing || (nn::swkbd::GetStateInputForm() != nn::swkbd::State::Hidden && ++closingFrames < 120)) {
            // Pass it the controllers, with the GamePad's touch calibrated like the menus use it
            // Input is also scanned for the menus, so buttons still held from here aren't new presses there
            getInputPress();
            VPADStatus gamepad = vpad;
            gamepad.tpNormal = touch;
            nn::swkbd::ControllerInfo info;
            info.vpad = &gamepad;
            for (int i = 0; i < 4; i++)
                info.kpad[i] = kpadRead[i] ? &kpad[i] : nullptr;
            nn::swkbd::Calc(info);
            if (nn::swkbd::IsNeedCalcSubThreadFont())
                nn::swkbd::CalcSubThreadFont();
            if (nn::swkbd::IsNeedCalcSubThreadPredict())
                nn::swkbd::CalcSubThreadPredict();

            // Take the text when confirmed, or leave it when canceled
            if (!closing && (nn::swkbd::IsDecideOkButton(nullptr) || nn::swkbd::IsDecideCancelButton(nullptr))) {
                if (nn::swkbd::IsDecideOkButton(nullptr)) {
                    const char16_t *result = nn::swkbd::GetInputFormString();
                    text = result ? result : u"";
                    decided = true;
                }
                nn::swkbd::DisappearInputForm();
                closing = true;
            }

            // Draw the keyboard on the GamePad, then copy it to the TV, where the system only draws it for remotes
            // The TV reads the GamePad's frame as a texture, set up each frame since WHB reallocates the frame when
            // getting the foreground back
            startFrame(palette[0]);
            drawKeyboard(false);
            GX2ColorBuffer *drcTarget = WHBGfxGetDRCColourBuffer();
            GX2Texture drcTexture;
            memset(&drcTexture, 0, sizeof(drcTexture));
            drcTexture.surface = drcTarget->surface;
            drcTexture.surface.use = GX2_SURFACE_USE_TEXTURE;
            drcTexture.viewNumSlices = 1;
            drcTexture.compMap = 0x00010203;
            GX2InitTextureRegs(&drcTexture);
            GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                drcTarget->surface.image, drcTarget->surface.imageSize);
            WHBGfxBeginRenderTV();
            GX2SetAttribBuffer(0, sizeof(composePositions), 2 * sizeof(float), composePositions);
            GX2SetAttribBuffer(1, sizeof(composeCoords), 2 * sizeof(float), composeCoords);
            GX2SetAttribBuffer(2, sizeof(composeColors), 4 * sizeof(float), composeColors);
            drawQuad(&drcTexture, true);
            drawKeyboard(true);
            WHBGfxBeginRenderDRC();
            endFrame();
        }
        nn::swkbd::Destroy();
    }

    // Free the keyboard's memory
    FSDelClient(client, FS_ERROR_FLAG_NONE);
    MEMFreeToDefaultHeap(client);
    MEMFreeToDefaultHeap(work);
    return decided;
}

// The GamePad's microphone, which the system fills a ring buffer from
const int ConsoleUI::micRate = 32000;
const size_t MIC_SAMPLES = 0x2800; // The smallest ring buffer the system takes
MICHandle micHandle = -1;
MICWorkMemory micWork;
int micRetry = 0; // Frames until opening it is tried again after failing

size_t ConsoleUI::getMicSamples(int16_t *out, size_t max) {
    // Open the microphone the first time it's needed, trying again every second if that fails
    if (micHandle < 0) {
        if (micRetry > 0) {
            micRetry--;
            return 0;
        }
        micRetry = 60;
        if (!micWork.sampleBuffer) {
            micWork.sampleMaxCount = MIC_SAMPLES;
            micWork.sampleBuffer = MEMAllocFromDefaultHeapEx(MIC_SAMPLES * sizeof(int16_t), 0x40);
            if (!micWork.sampleBuffer) return 0;
        }
        MICError error = MIC_ERROR_INIT;
        micHandle = MICInit(MIC_INSTANCE_0, 0, &micWork, &error);
        if (micHandle < 0 || error != MIC_ERROR_OK) {
            micHandle = -1;
            return 0;
        }
        if (MICOpen(micHandle) != MIC_ERROR_OK) {
            closeMic();
            return 0;
        }
    }

    // Copy the samples recorded since the last call, closing the microphone if it stopped working
    MICStatus status;
    if (MICGetStatus(micHandle, &status) != MIC_ERROR_OK) {
        closeMic();
        return 0;
    }
    size_t count = std::min<size_t>(std::max(status.availableData, 0), max);
    const int16_t *samples = (const int16_t*)micWork.sampleBuffer;
    for (size_t i = 0; i < count; i++)
        out[i] = samples[(status.bufferPos + i) % MIC_SAMPLES];
    if (count) MICSetDataConsumed(micHandle, count);
    return count;
}

void ConsoleUI::closeMic() {
    // Stop recording from the microphone if it's open
    if (micHandle < 0) return;
    MICClose(micHandle);
    MICUninit(micHandle);
    micHandle = -1;
}

// The GamePad's camera, which shows the DSi's cameras the player while one of them is on
CAMHandle camHandle = -1;
void *camWork = nullptr;
void *camBuffers[2] = {};
CAMSurface camSurfaces[2];
uint32_t camFrame[DsiCamera::WIDTH * DsiCamera::HEIGHT / 2];
int camRetry = 0; // Frames until opening it is tried again after failing

void cameraEvent(CAMEventData *event) {
    // Convert a decoded NV12 picture to the YUV422 the DSi's sensors send, two pixels a word, and queue its surface again
    if (event->eventType != CAMERA_DECODE_DONE) return;
    const uint8_t *y = (const uint8_t*)event->decode.surfaceBuffer;
    const uint8_t *uv = y + CAMERA_Y_BUFFER_SIZE;
    if (!event->decode.failed) {
        for (int row = 0; row < CAMERA_HEIGHT; row++) {
            const uint8_t *ys = &y[row * CAMERA_PITCH], *uvs = &uv[(row / 2) * CAMERA_PITCH];
            uint32_t *out = &camFrame[row * CAMERA_WIDTH / 2];
            for (int x = 0; x < CAMERA_WIDTH; x += 2)
                out[x / 2] = ys[x] | (uvs[x] << 8) | (ys[x + 1] << 16) | (uint32_t(uvs[x + 1]) << 24);
        }
        DsiCamera::sendFrame(camFrame);
    }
    for (int i = 0; i < 2; i++)
        if (camBuffers[i] == event->decode.surfaceBuffer)
            CAMSubmitTargetSurface(camHandle, &camSurfaces[i]);
}

void closeCamera() {
    // Stop the camera if it's open and free its memory
    if (camHandle >= 0) {
        CAMClose(camHandle);
        CAMExit(camHandle);
        camHandle = -1;
    }
    for (int i = 0; i < 2; i++) {
        if (camBuffers[i]) MEMFreeToDefaultHeap(camBuffers[i]);
        camBuffers[i] = nullptr;
    }
    if (camWork) MEMFreeToDefaultHeap(camWork);
    camWork = nullptr;
}

void updateCamera() {
    // Follow whether the DSi wants pictures, opening the camera when it does and trying again every second on failure
    bool wanted = DsiCamera::streaming.load();
    if (!wanted) {
        closeCamera();
        camRetry = 0;
        return;
    }
    if (camHandle >= 0) return;
    if (camRetry > 0) {
        camRetry--;
        return;
    }
    camRetry = 60;

    // Set up 15 pictures a second, which is what the DSi's cameras give, without showing them on the GamePad
    CAMSetupInfo setup = {};
    setup.streamInfo.type = CAMERA_STREAM_TYPE_1;
    setup.streamInfo.width = CAMERA_WIDTH;
    setup.streamInfo.height = CAMERA_HEIGHT;
    int32_t size = CAMGetMemReq(&setup.streamInfo);
    if (size <= 0 || !(camWork = MEMAllocFromDefaultHeapEx(size, 0x100))) return closeCamera();
    setup.workMem.size = size;
    setup.workMem.pMem = camWork;
    setup.eventHandler = cameraEvent;
    setup.mode.forceDrc = CAMERA_FORCE_DRC_OFF;
    setup.mode.fps = CAMERA_FPS_15;
    setup.threadAffinity = OS_THREAD_ATTRIB_AFFINITY_CPU0;
    CAMError error = CAMERA_ERROR_OK;
    camHandle = CAMInit(0, &setup, &error);
    if (camHandle < 0 || error != CAMERA_ERROR_OK) {
        camHandle = -1;
        return closeCamera();
    }

    // Give it two surfaces to decode into, taking turns
    for (int i = 0; i < 2; i++) {
        if (!(camBuffers[i] = MEMAllocFromDefaultHeapEx(CAMERA_YUV_BUFFER_SIZE, CAMERA_YUV_BUFFER_ALIGNMENT)))
            return closeCamera();
        camSurfaces[i] = {};
        camSurfaces[i].surfaceSize = CAMERA_YUV_BUFFER_SIZE;
        camSurfaces[i].surfaceBuffer = camBuffers[i];
        camSurfaces[i].height = CAMERA_HEIGHT;
        camSurfaces[i].width = CAMERA_WIDTH;
        camSurfaces[i].pitch = CAMERA_PITCH;
        camSurfaces[i].alignment = CAMERA_YUV_BUFFER_ALIGNMENT;
    }
    if (CAMOpen(camHandle) != CAMERA_ERROR_OK) return closeCamera();
    for (int i = 0; i < 2; i++)
        CAMSubmitTargetSurface(camHandle, &camSurfaces[i]);
}

#ifdef AUDIO_CAPTURE
// What the mixer hands each of its callbacks: a pointer for every channel of every device, and how much is there
struct FinalMix {
    int32_t **data;
    uint16_t channels, samples, devices, channelsOut;
};

void captureTvMix(void *mix) {
    // Keep the TV's left and right as they're about to be played
    FinalMix *m = (FinalMix*)mix;
    if (m->channels >= 2) FrameLog::audioFinal(0, m->data[0], m->data[1], m->samples);
}

void captureDrcMix(void *mix) {
    // The same for the first GamePad
    FinalMix *m = (FinalMix*)mix;
    if (m->channels >= 2) FrameLog::audioFinal(1, m->data[0], m->data[1], m->samples);
}
#endif

void outputAudio(void *, uint8_t *buffer, int length) {
    // Refill the audio buffer as a callback
    uint32_t *samples = (uint32_t*)buffer;
    int count = length / sizeof(uint32_t);
    ConsoleUI::fillAudioBuffer(samples, count, 32768);

    // The core's samples have the right channel in their high half, which comes first in memory here, and the left
    // channel is what has to come first
    for (int i = 0; i < count; i++)
        samples[i] = (samples[i] << 16) | (samples[i] >> 16);
}

int runUi(int, void *) {
    // Initialize various things
    WHBGfxInit();
    drcStartMode = drcBufferMode = GX2GetSystemDRCMode();
    KPADInit();
    WPADEnableURCC(TRUE);
    WHBMountSdCard();
    gpLayout.update(854, 480, false);

    // Get the current TV render dimensions
    switch (GX2GetSystemTVScanMode()) {
    case GX2_TV_SCAN_MODE_480I:
    case GX2_TV_SCAN_MODE_480P:
        // WHBGfxInit gives a TV set to 4:3 a 640x480 buffer instead of a wide one
        tvWidth = (GX2GetSystemTVAspectRatio() == GX2_ASPECT_RATIO_4_3) ? 640 : 854;
        tvHeight = 480;
        break;

    case GX2_TV_SCAN_MODE_1080I:
    case GX2_TV_SCAN_MODE_1080P:
        tvWidth = 1920;
        tvHeight = 1080;
        break;

    default:
        tvWidth = 1280;
        tvHeight = 720;
        break;
    }

    // Initialize the shader
    WHBGfxLoadGFDShaderGroup(&group, 0, shader_wiiu_gsh);
    WHBGfxInitShaderAttribute(&group, "position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&group, "tex_coords", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&group, "vtx_color", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitFetchShader(&group);
    WHBGfxLoadGFDShaderGroup(&screenGroup, 0, screen_wiiu_gsh);
    WHBGfxInitShaderAttribute(&screenGroup, "position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&screenGroup, "tex_coords", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&screenGroup, "vtx_color", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitFetchShader(&screenGroup);
    WHBGfxLoadGFDShaderGroup(&packedGroup, 0, screen_packed_wiiu_gsh);
    WHBGfxInitShaderAttribute(&packedGroup, "position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&packedGroup, "tex_coords", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&packedGroup, "vtx_color", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitFetchShader(&packedGroup);
    WHBGfxLoadGFDShaderGroup(&gbaGroup, 0, screen_gba_wiiu_gsh);
    WHBGfxInitShaderAttribute(&gbaGroup, "position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&gbaGroup, "tex_coords", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&gbaGroup, "vtx_color", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitFetchShader(&gbaGroup);
    WHBGfxLoadGFDShaderGroup(&blendGroup, 0, screen_blend_wiiu_gsh);
    WHBGfxInitShaderAttribute(&blendGroup, "position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&blendGroup, "tex_coords", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&blendGroup, "vtx_color", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitFetchShader(&blendGroup);

    // Make a blank high-res 3D texture for screens without one, so their native 3D shows
    static uint32_t blank = 0;
    ConsoleUI::blankHiRes = ConsoleUI::createTexture(&blank, 1, 1);
    initCompose();

    // Initialize the position buffer
    posBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
        GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    posBuffer.elemSize = 2 * sizeof(float);
    posBuffer.elemCount = 8 * MAX_DRAWS;
    GX2RCreateBuffer(&posBuffer);

    // Initialize the texture coordinate buffer
    texBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
        GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    texBuffer.elemSize = 2 * sizeof(float);
    texBuffer.elemCount = 4 * MAX_DRAWS;
    GX2RCreateBuffer(&texBuffer);

    // Initialize the vertex color buffer
    colBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
        GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    colBuffer.elemSize = 4 * sizeof(float);
    colBuffer.elemCount = 4 * MAX_DRAWS;
    GX2RCreateBuffer(&colBuffer);

    // Initialize samplers for each texture filter mode
    GX2InitSampler(&samplers[0], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSampler(&samplers[1], GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

    // Configure shading and blending for the TV
    WHBGfxBeginRenderTV();
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);

    // Configure shading and blending for the gamepad
    WHBGfxBeginRenderDRC();
    GX2SetFetchShader(&group.fetchShader);
    GX2SetVertexShader(group.vertexShader);
    GX2SetPixelShader(group.pixelShader);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.0f);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
        GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);

    // Set up the GPU 3D renderer, which has its own GX2 state
    initGpu3D();
#ifdef DRC_DEBUG
    GX2SetDRCConnectCallback(0, drcConnected);
#endif

    // Report display refreshes from core 2, above the other threads there so the emulator isn't held back
    vsyncThread = WorkerThread::createThread(reportVsyncs, nullptr, 0x4000, 11, OS_THREAD_ATTRIB_AFFINITY_CPU2);

    // Initialize audio output
    SDL_Init(SDL_INIT_AUDIO);
    SDL_AudioSpec AudioSettings, ObtainedSettings;
    AudioSettings.freq = 32768;
    AudioSettings.format = AUDIO_S16MSB;
    AudioSettings.channels = 2;
    // Small buffers mean the emulator can fall further behind before output runs out of samples, like while loading
    AudioSettings.samples = 512;
    AudioSettings.callback = &outputAudio;
    AudioSettings.userdata = nullptr;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(nullptr, 0, &AudioSettings, &ObtainedSettings, 0);
    SDL_PauseAudioDevice(id, 0);
#ifdef AUDIO_CAPTURE
    AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, captureTvMix);
    AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_DRC, captureDrcMix);
#endif

    // Initialize the UI and open the file browser
    std::string base = WHBGetSdCardMountPath();

    // Carry settings, states and system files over from the app's old name, once
    struct stat st;
    std::string oldDir = base + "/wiiu/apps/noodsu", newDir = base + "/wiiu/apps/maelle";
    if (stat(newDir.c_str(), &st) != 0 && stat(oldDir.c_str(), &st) == 0 && rename(oldDir.c_str(), newDir.c_str()) == 0)
        rename((newDir + "/noodsu.ini").c_str(), (newDir + "/maelle.ini").c_str());

#ifdef SHADER_TEST
    shaderTest();
#endif
    ConsoleUI::initialize(tvWidth, tvHeight, base, base + "/wiiu/apps/maelle/");
    ConsoleUI::fileBrowser();

    // Run the emulator until it exits, then close the game while the app still has the foreground
    ConsoleUI::mainLoop(nullptr, &gpLayout);
    ConsoleUI::closeMic();
    closeCamera();
    ConsoleUI::closeCore();

    // Close the audio, end the threads that report display refreshes and watch for hangs, and close the controllers
    SDL_CloseAudioDevice(id);
    SDL_Quit();
    appClosing = true;
    if (vsyncThread) OSJoinThread(vsyncThread, nullptr);
    if (hangThread) OSJoinThread(hangThread, nullptr);
    KPADShutdown();
    return 0;
}

void closeGraphics() {
    // Shut down graphics and unmount the SD card after the UI has finished, once the system has taken the foreground
    WHBGfxShutdown();
    WHBUnmountSdCard();
}

#ifdef THREAD_PROBE
int probeThreads(int, void *) {
    // Reply to any UDP packet on port 4406 plus this thread's core with every thread's state, the mutex it waits on,
    // and where it is, using only the OS's sockets and formatting so locks held by stuck threads can't block it
    // There's one on each core, to tell which cores still run after a freeze
    static std::atomic<bool> started(false);
    if (!started.exchange(true)) {
        ACInitialize();
        ACConnect();
        socket_lib_init();
    }
    else {
        OSSleepTicks(OSSecondsToTicks(5));
    }
    int udp = RPLWRAP(socket)(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int enabled = 1;
    if (udp < 0 || RPLWRAP(setsockopt)(udp, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0) return 0;
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(4406 + OSGetCoreId());
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (RPLWRAP(bind)(udp, (sockaddr*)&local, sizeof(local)) < 0) return 0;
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(4405);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char message[512];
    auto send = [&] {
        RPLWRAP(sendto)(udp, message, std::strlen(message), MSG_DONTWAIT, (sockaddr*)&destination, sizeof(destination));
    };
    for (uint32_t tick = 0;; tick++) {
        // Announce once at startup, then sleep in the receive until a snapshot is asked for
        if (tick) {
            char request;
            sockaddr_in sender = {};
            socklen_t length = sizeof(sender);
            if (RPLWRAP(recvfrom)(udp, &request, sizeof(request), 0, (sockaddr*)&sender, &length) < 0) continue;
            destination.sin_addr = sender.sin_addr;
        }
        __os_snprintf(message, sizeof(message), "Maelle thread probe %u on core %u, probe address %08X\n", tick,
            OSGetCoreId(), (uint32_t)&probeThreads);
        send();
        if (!tick) continue;

        // Walk the OS's list of active threads; other cores keep running, so check tags and bound the walk
        OSThread *thread = OSGetCurrentThread();
        for (int i = 0; thread->activeLink.prev && i < 128; i++) thread = thread->activeLink.prev;
        for (int i = 0; thread && i < 128; i++, thread = thread->activeLink.next) {
            if (thread->tag != OS_THREAD_TAG) break;
            uint32_t sp = thread->context.gpr[1];
            int length = __os_snprintf(message, sizeof(message), "thread %08X %s state %u priority %d/%d affinity %u "
                "mutex %08X pc %08X lr %08X sp %08X\nstack:", (uint32_t)thread, thread->name ? thread->name : "?",
                thread->state, thread->priority, thread->basePriority, thread->attr & 7, (uint32_t)thread->mutex,
                thread->context.srr0, thread->context.lr, sp);
            for (int frame = 0; frame < 10 && sp >= 0x10000000 && sp < 0x50000000 && !(sp & 3); frame++) {
                uint32_t next = *(uint32_t*)sp;
                if (next <= sp || next >= 0x50000000 || (next & 3)) break;
                length += __os_snprintf(message + length, sizeof(message) - length, " %08X", *(uint32_t*)(next + 4));
                sp = next;
            }
            __os_snprintf(message + length, sizeof(message) - length, "\n");
            send();
        }
        __os_snprintf(message, sizeof(message), "End of thread probe %u on core %u\n", tick, OSGetCoreId());
        send();
    }
    return 0;
}
#endif

#ifdef HEARTBEAT
int logHeartbeat(int, void *) {
    // Append the progress counters to a file a few times a second, closing it every time so it survives a freeze
    std::string path = std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/maelle/heartbeat.log";
    FILE *file = fopen(path.c_str(), "w");
    if (file) fclose(file);
    uint32_t last[HB_COUNT] = {};
    for (uint32_t tick = 0;; tick++) {
        OSSleepTicks(OSMillisecondsToTicks(250));

        // Stop writing once the system wants the foreground or the UI is done, since file access then can keep the app
        // from closing
        if (releaseRequested.load() || uiParked || uiDone) continue;
        volatile uint32_t *v = Heartbeat::values;
        std::lock_guard<FileLock> guard(fileLock());
        if (!(file = fopen(path.c_str(), "a"))) continue;
        fprintf(file, "%u: frames %u task %u wait %u | jit %08X in %u entries %u | 2D %u 3D %u | ui %u stage %u | audio %u"
            " | gpu %u stage %u 3dcnt %04X\n", tick, v[HB_EMU_FRAMES], v[HB_EMU_TASK], v[HB_EMU_WAIT], v[HB_JIT_BLOCK],
            v[HB_JIT_INSIDE], v[HB_JIT_ENTRIES], v[HB_2D_LINES], v[HB_3D_LINES], v[HB_UI_FRAMES], v[HB_UI_STAGE],
            v[HB_AUDIO], v[HB_GPU_FRAMES], v[HB_GPU_STAGE], v[HB_GPU_DISP3DCNT]);

        // Show the timings as microseconds per frame since the last line, with rates over the real time between lines
        // The time is taken again when the counters are, after writing, so slow writes don't skew the rates
        static OSTime lastTime = OSGetTime();
        float seconds = std::max<int64_t>(OSTicksToMicroseconds(OSGetTime() - lastTime), 1) / 1000000.0f;
        uint32_t frames = v[HB_EMU_FRAMES] - last[HB_EMU_FRAMES], gpuFrames = v[HB_GPU_FRAMES] - last[HB_GPU_FRAMES];
        auto per = [&](int slot, uint32_t count) { return count ? (v[slot] - last[slot]) / count : 0; };
        auto perTicks = [&](int slot, uint32_t count) { return uint32_t(OSTicksToMicroseconds(uint64_t(per(slot, count)))); };
        uint32_t uiFrames = v[HB_UI_FRAMES] - last[HB_UI_FRAMES];
        fprintf(file, "   %.0f ms | ui fps %.1f, new frames %.1f, skipped %u, repeated %u: get frame %u (convert %u), textures %u, draw %u, "
            "finish %u us\n", seconds * 1000, uiFrames / seconds, (v[HB_UI_NEW_FRAMES] - last[HB_UI_NEW_FRAMES]) / seconds,
            v[HB_UI_SKIPPED] - last[HB_UI_SKIPPED], v[HB_UI_REPEATS] - last[HB_UI_REPEATS],
            per(HB_UI_GETFRAME_US, uiFrames), per(HB_UI_CONVERT_US, uiFrames), per(HB_UI_TEXTURE_US, uiFrames), per(HB_UI_DRAW_US, uiFrames),
            per(HB_UI_END_US, uiFrames));
        fprintf(file, "   fps %.1f | gpu frames %u: build %u (textures %u) lock %u draw %u convert %u us | "
            "emu waits gpu %u 2D %u | 2D waits 3D %u us | hi-res copy %u us\n", frames / seconds, gpuFrames, per(HB_GPU_BUILD_US, gpuFrames),
            perTicks(HB_GPU_TEXTURE_US, gpuFrames), per(HB_GPU_LOCK_US, gpuFrames), per(HB_GPU_DRAW_US, gpuFrames),
            per(HB_GPU_CONVERT_US, gpuFrames), per(HB_EMU_3D_WAIT_US, frames), per(HB_2D_JOIN_US, frames),
            per(HB_3D_WAIT_US, frames), per(HB_EMU_HIRES_US, frames));
        uint32_t textureUs = perTicks(HB_GPU_TEXTURE_US, gpuFrames), vertexUs = perTicks(HB_GPU_VERTEX_US, gpuFrames);
        uint32_t fanUs = perTicks(HB_GPU_FAN_US, gpuFrames), setupUs = perTicks(HB_GPU_SETUP_US, gpuFrames);
        uint32_t sendUs = perTicks(HB_GPU_SEND_US, gpuFrames);
        fprintf(file, "   gpu draw timeline per frame (GPU_TIMELINE only): earlier work done %u, polygons submitted %u, "
            "polygons done %u, all submitted %u, all done %u us | draws %u, vertices %u\n", per(HB_GPU_PRIOR_US, gpuFrames), per(HB_GPU_MAIN_SUBMIT_US, gpuFrames),
            per(HB_GPU_MAIN_DONE_US, gpuFrames), per(HB_GPU_SUBMIT_US, gpuFrames), per(HB_GPU_DRAW_US, gpuFrames),
            per(HB_GPU_DRAWS, gpuFrames), per(HB_GPU_VERTICES, gpuFrames));
#ifdef GPU_TIMELINE
        fprintf(file, "   ui gpu per frame (TV %ux%u): waits for earlier work %u, clear %u, compose %u, frames %u, screens on "
            "TV %u gamepad %u, rest %u, copy TV %u gamepad %u us\n", tvWidth, tvHeight, per(HB_UI_GPU_PRIOR_US, uiFrames),
            per(HB_UI_GPU_CLEAR_US, uiFrames), per(HB_UI_GPU_COMPOSE_US, uiFrames), per(HB_UI_GPU_FRAMES_US, uiFrames),
            per(HB_UI_GPU_SCREENS_TV_US, uiFrames), per(HB_UI_GPU_SCREENS_DRC_US, uiFrames), per(HB_UI_GPU_REST_US, uiFrames),
            per(HB_UI_GPU_COPY_TV_US, uiFrames), per(HB_UI_GPU_COPY_DRC_US, uiFrames));
        fprintf(file, "   gpu passes per frame: multisampled %u, polygons %u, crack marking %u, crack filling %u, edges %u, fog %u, anti-alias %u, pack and copy %u us"
            " | frames multisampled %u, high-res %u\n", per(HB_GPU_PASS_MSAA_US, gpuFrames), per(HB_GPU_PASS_POLY_US, gpuFrames),
            per(HB_GPU_PASS_MARK_US, gpuFrames), per(HB_GPU_PASS_FILL_US, gpuFrames), per(HB_GPU_PASS_EDGE_US, gpuFrames), per(HB_GPU_PASS_FOG_US, gpuFrames), per(HB_GPU_PASS_AA_US, gpuFrames),
            per(HB_GPU_PASS_PACK_US, gpuFrames), v[HB_GPU_MSAA_FRAMES] - last[HB_GPU_MSAA_FRAMES],
            v[HB_GPU_HIRES_FRAMES] - last[HB_GPU_HIRES_FRAMES]);
        fprintf(file, "   gpu polygon pass per frame: clear %u | draws by shader, us and vertices: color %u %u, modulate %u %u, "
            "decal %u %u, translucent modulate %u %u, toon %u %u, shadow %u %u\n", per(HB_GPU_PASS_CLEAR_US, gpuFrames),
            per(HB_GPU_DRAW_COLOR_US, gpuFrames), per(HB_GPU_VTX_COLOR, gpuFrames), per(HB_GPU_DRAW_MODULATE_US, gpuFrames),
            per(HB_GPU_VTX_MODULATE, gpuFrames), per(HB_GPU_DRAW_DECAL_US, gpuFrames), per(HB_GPU_VTX_DECAL, gpuFrames),
            per(HB_GPU_DRAW_MODTRANS_US, gpuFrames), per(HB_GPU_VTX_MODTRANS, gpuFrames), per(HB_GPU_DRAW_TOON_US, gpuFrames),
            per(HB_GPU_VTX_TOON, gpuFrames), per(HB_GPU_DRAW_SHADOW_US, gpuFrames), per(HB_GPU_VTX_SHADOW, gpuFrames));
#endif
        fprintf(file, "   gpu build parts per frame: before polygons %u, setup %u, vertices %u, triangles %u, sending %u, "
            "after polygons %u, textures %u, rest %d us\n", per(HB_GPU_PRELOOP_US, gpuFrames), setupUs, vertexUs, fanUs, sendUs,
            per(HB_GPU_POSTLOOP_US, gpuFrames), textureUs, int(per(HB_GPU_BUILD_US, gpuFrames)) -
            int(per(HB_GPU_PRELOOP_US, gpuFrames) + setupUs + vertexUs + fanUs + sendUs + per(HB_GPU_POSTLOOP_US, gpuFrames) +
            textureUs));
        fprintf(file, "   gpu textures per frame: hash %u, decode %u, expire %u us | in total: decoded %u, from pool %u, "
            "created %u in %u us (GX2 setup %u, memory %u), freed %u | pool %u of %u KB\n", perTicks(HB_GPU_TEX_HASH_US, gpuFrames),
            perTicks(HB_GPU_TEX_DECODE_US, gpuFrames), perTicks(HB_GPU_TEX_EXPIRE_US, gpuFrames),
            v[HB_GPU_TEX_DECODES] - last[HB_GPU_TEX_DECODES], v[HB_GPU_TEX_POOLED] - last[HB_GPU_TEX_POOLED],
            v[HB_GPU_TEX_ALLOCS] - last[HB_GPU_TEX_ALLOCS],
            uint32_t(OSTicksToMicroseconds(v[HB_GPU_TEX_SETUP_US] - last[HB_GPU_TEX_SETUP_US] + v[HB_GPU_TEX_ALLOC_US] - last[HB_GPU_TEX_ALLOC_US])),
            uint32_t(OSTicksToMicroseconds(v[HB_GPU_TEX_SETUP_US] - last[HB_GPU_TEX_SETUP_US])),
            uint32_t(OSTicksToMicroseconds(v[HB_GPU_TEX_ALLOC_US] - last[HB_GPU_TEX_ALLOC_US])),
            v[HB_GPU_TEX_FREES] - last[HB_GPU_TEX_FREES], v[HB_GPU_TEX_POOL_KB], v[HB_GPU_TEX_POOL_LIMIT_KB]);
        fprintf(file, "   2D lines drawn by the emulator per frame: A %u, B %u | cores: 2D %u, geometry %u | "
            "ROM misses %u, waited %u us\n", per(HB_2D_EMU_LINES_A, frames), per(HB_2D_EMU_LINES_B, frames),
            v[HB_2D_CORE], v[HB_GEOM_CORE], v[HB_ROM_MISSES] - last[HB_ROM_MISSES],
            v[HB_ROM_WAIT_US] - last[HB_ROM_WAIT_US]);
        fprintf(file, "   ROM: emulator waits for block %d, loader at block %u stage %u\n", int(v[HB_ROM_WAIT_BLOCK]) - 1,
            v[HB_ROM_LOAD_BLOCK], v[HB_ROM_LOAD_STAGE]);

        // Show where the CPUs are, read without stopping them, to see where a game is stuck
        if (Core *core = ConsoleUI::core)
            fprintf(file, "   CPUs: ARM9 PC %08X halted %u, ARM7 PC %08X halted %u, GBA %d\n", core->interpreter[0].getPC(),
                core->interpreter[0].halted, core->interpreter[1].getPC(), core->interpreter[1].halted, core->gbaMode);

        // Show which 3D features were used, with polygon counts per 3D frame
        uint32_t frames3D = v[HB_3D_FRAMES] - last[HB_3D_FRAMES];
        if (frames3D) {
            uint32_t flags = v[HB_3D_FLAGS];
            v[HB_3D_FLAGS] = 0;
            fprintf(file, "   3D features:%s%s%s%s%s%s%s | polygons per frame %u: toon %u highlight %u shadow masks %u "
                "shadows %u wireframe %u\n", (flags & BIT(1)) ? " highlight-mode" : "", (flags & BIT(2)) ? " alpha-test" : "",
                (flags & BIT(4)) ? " anti-alias" : "", (flags & BIT(5)) ? " edge-marking" : "", (flags & BIT(7)) ? " fog" : "",
                (flags & BIT(6)) ? " fog-alpha-only" : "", (flags & BIT(14)) ? " clear-bitmap" : "",
                per(HB_3D_POLYS, frames3D), per(HB_3D_TOON, frames3D), per(HB_3D_HIGHLIGHT, frames3D),
                per(HB_3D_SHADOW_MASKS, frames3D), per(HB_3D_SHADOWS, frames3D), per(HB_3D_WIREFRAME, frames3D));
        }
#ifdef PROFILE_EMU
        // Show where the emulator thread spent each frame on average, then list the frames that took too long without
        // counting the display wait, which delay the next one; all times are in microseconds, and CPU time includes
        // compiling, while tasks include the 2D waits, joins and frame copies
        auto frameLine = [&](const char *label, const EmuFrameRecord &r, uint32_t n) {
            const uint32_t *t = r.ticks, *c = r.counts;
            fprintf(file, "   %s: wall %u display %u | arm9 %u arm7 %u tasks %u compile %u geometry %u audio %u | "
                "2D line waits %u, join A %u B %u, 3D join %u, frame copy %u (hi-res %u) | jit compiled %u retired %u "
                "resets %u code writes %u, interpreted %u | gpu frames %u build %u draw %u, 2D waits 3D %u\n", label,
                r.wall / n, t[EP_DISPLAY] / n, (t[EP_ARM9] + t[EP_BOTH] - t[EP_BOTH_ARM7]) / n,
                (t[EP_ARM7] + t[EP_BOTH_ARM7]) / n, t[EP_TASKS] / n, t[EP_COMPILE] / n, t[EP_GEOM] / n, t[EP_SPU] / n,
                t[EP_WAIT2D] / n, t[EP_JOIN2D_A] / n, t[EP_JOIN2D_B] / n, t[EP_JOIN3D] / n, t[EP_FRAMECOPY] / n,
                t[EP_HIRES] / n, c[EC_JIT_COMPILED] / n, c[EC_JIT_RETIRED] / n, c[EC_JIT_RESETS], c[EC_JIT_WRITES] / n,
                c[EC_INTERP9] / n, r.gpuFrames, r.gpuBuild / n, r.gpuDraw / n, r.wait3D / n);
        };
        static uint32_t lastRecord = 0;
        uint32_t count = EmuProfile::recordCount;
        if (count - lastRecord > EmuProfile::RECORDS) lastRecord = count - EmuProfile::RECORDS;
        EmuFrameRecord sum = {};
        std::vector<uint32_t> slow;
        for (uint32_t i = lastRecord; i != count; i++) {
            const EmuFrameRecord &r = EmuProfile::records[i % EmuProfile::RECORDS];
            sum.wall += r.wall;
            for (int j = 0; j < EP_COUNT; j++) sum.ticks[j] += r.ticks[j];
            for (int j = 0; j < EC_COUNT; j++) sum.counts[j] += r.counts[j];
            sum.gpuFrames += r.gpuFrames;
            sum.gpuBuild += r.gpuBuild;
            sum.gpuDraw += r.gpuDraw;
            sum.wait3D += r.wait3D;
            if (r.wall - r.ticks[EP_DISPLAY] > 16000 || r.wall > 17500) slow.push_back(i);
        }
        if (count != lastRecord) {
            fprintf(file, "   emulator frames %u, %zu slow\n", count - lastRecord, slow.size());
            frameLine("average", sum, count - lastRecord);
            for (size_t i = 0; i < slow.size() && i < 40; i++) {
                const EmuFrameRecord &r = EmuProfile::records[slow[i] % EmuProfile::RECORDS];
                char label[32];
                snprintf(label, sizeof(label), "slow frame %u", r.frame);
                frameLine(label, r, 1);
            }
        }
        lastRecord = count;
        FrameTrace::write((std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/maelle/trace.bin").c_str());
#endif
#ifdef JIT_SPLIT
        // Show the total time in each step of compiling blocks so far
        fprintf(file, "   jit split: blocks %u, bytes %u | total %llu, protect %llu, switch rw %llu, emit %llu, link %llu, "
            "flush %llu, switch rx %llu, invalidate %llu | new %llu, reuse %llu, decode %llu, retire %llu, track %llu, "
            "generate %llu us\n", jitSplitBlocks, jitSplitBytes,
            OSTicksToMicroseconds(jitSplit[JS_TOTAL]), OSTicksToMicroseconds(jitSplit[JS_PROTECT]),
            OSTicksToMicroseconds(jitSplit[JS_SWITCH_RW]), OSTicksToMicroseconds(jitSplit[JS_EMIT]),
            OSTicksToMicroseconds(jitSplit[JS_LINK]), OSTicksToMicroseconds(jitSplit[JS_FLUSH]),
            OSTicksToMicroseconds(jitSplit[JS_SWITCH_RX]), OSTicksToMicroseconds(jitSplit[JS_INVALIDATE]),
            OSTicksToMicroseconds(jitSplit[JS_NEW]), OSTicksToMicroseconds(jitSplit[JS_REUSE]),
            OSTicksToMicroseconds(jitSplit[JS_DECODE]),
            OSTicksToMicroseconds(jitSplit[JS_RETIRE]), OSTicksToMicroseconds(jitSplit[JS_TRACK]),
            OSTicksToMicroseconds(jitSplit[JS_GENERATE]));
#endif
        // Show the heap use, to spot leaks
        struct mallinfo info = mallinfo();
        fprintf(file, "   memory: heap %u KB, used %u KB, free %u KB | gpu textures %u, %u KB\n", info.arena >> 10,
            info.uordblks >> 10, info.fordblks >> 10, v[HB_GPU_TEXTURES], v[HB_GPU_TEXTURE_KB]);
        fclose(file);
        lastTime = OSGetTime();
        for (int i = 0; i < HB_COUNT; i++)
            last[i] = v[i];
    }
    return 0;
}
#endif

#ifdef FRAME_LOG
int logFrames(int, void *) {
    // Write what the frame log collected every few seconds, unless the system wants the foreground or the UI is done
    std::string path = std::string(WHBGetSdCardMountPath()) + "/wiiu/apps/maelle/framelog.bin";
    while (true) {
        OSSleepTicks(OSMillisecondsToTicks(2000));
        if (releaseRequested.load() || uiParked || uiDone) continue;
#ifdef AUDIO_CAPTURE
        if (FrameLog::shouldDeferWrite()) continue;
#endif
        std::lock_guard<FileLock> guard(fileLock());
        FrameLog::write(path.c_str());
    }
    return 0;
}
#endif

// Show where a crash happened on an error screen instead of freezing, with this function's own address so the others
// can be matched to the build's symbols; it formats with the OS, since C runtime locks might be held, and doesn't walk
// the stack, which might be damaged
static BOOL showCrash(OSContext *context, const char *type) {
    char message[512];
    __os_snprintf(message, sizeof(message), "Maelle crashed (%s) on core %u\n"
        "PC %08X LR %08X address %08X status %08X\nSP %08X handler %08X\n"
        "r3 %08X r4 %08X r5 %08X r6 %08X\nr7 %08X r8 %08X r9 %08X r10 %08X\n",
        type, OSGetCoreId(), context->srr0, context->lr, context->dar, context->srr1,
        context->gpr[1], (uint32_t)&showCrash, context->gpr[3], context->gpr[4], context->gpr[5], context->gpr[6],
        context->gpr[7], context->gpr[8], context->gpr[9], context->gpr[10]);

    // Follow the stack's back chain for the functions that led there, as far as it stays in the app's memory
    size_t length = strlen(message);
    length += __os_snprintf(&message[length], sizeof(message) - length, "stack:");
    uint32_t sp = context->gpr[1];
    for (int i = 0; i < 8 && sp - 0x10000000 < 0x40000000 && !(sp & 3) && length < sizeof(message) - 10; i++) {
        uint32_t frame = *(uint32_t*)(uintptr_t)sp;
        if (frame - 0x10000000 >= 0x40000000 || (frame & 3)) break;
        length += __os_snprintf(&message[length], sizeof(message) - length, " %08X", *(uint32_t*)(uintptr_t)(frame + 4));
        sp = frame;
    }
    OSFatal(message);
    return FALSE;
}

// Show failed assertions on an error screen too, since the C library's abort() leaves the console frozen with garbage
// on the screen and no message
extern "C" void __assert_func(const char *file, int line, const char *function, const char *expression) {
    char message[512];
    __os_snprintf(message, sizeof(message), "Maelle assertion failed on core %u\n%s\nin %s\n%s:%d\n", OSGetCoreId(),
        expression, function ? function : "?", file, line);
    OSFatal(message);
    while (true);
}

int main() {
    ProcUIInit(OSSavesDone_ReadyToRelease);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI,
        [](OSContext *context) { return showCrash(context, "DSI"); });
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI,
        [](OSContext *context) { return showCrash(context, "ISI"); });
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM,
        [](OSContext *context) { return showCrash(context, "program"); });
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT,
        [](OSContext *context) { return showCrash(context, "alignment"); });

    // Run Game Boy ROMs in MiGoomba, the GB emulator for GBA built into the app, as built for it
    CartridgeGba::setGbEmulator(migoomba_maelle_gba, migoomba_maelle_gba_end - migoomba_maelle_gba);

    // Run the UI on core 2 so it doesn't compete with the JIT's emulator thread, which has to use core 1
    // The main thread can't be moved there, since running threads can't change cores
#ifdef HEARTBEAT
    WHBMountSdCard();
    WorkerThread::createThread(logHeartbeat, nullptr, 0x10000, 20, OS_THREAD_ATTRIB_AFFINITY_CPU2);
#endif
#ifdef FRAME_LOG
    WHBMountSdCard();
    WorkerThread::createThread(logFrames, nullptr, 0x10000, 20, OS_THREAD_ATTRIB_AFFINITY_CPU2);
#endif
#ifdef THREAD_PROBE
    // Above the other threads' priorities, so they can answer even if threads below them are busy
    WorkerThread::createThread(probeThreads, nullptr, 0x10000, 14, OS_THREAD_ATTRIB_AFFINITY_CPU0);
    WorkerThread::createThread(probeThreads, nullptr, 0x10000, 1, OS_THREAD_ATTRIB_AFFINITY_CPU1);
    WorkerThread::createThread(probeThreads, nullptr, 0x10000, 1, OS_THREAD_ATTRIB_AFFINITY_CPU2);
#endif
    // Watch for hangs on core 2, where writing the report is safe, above the other threads so it still runs
    hangThread = WorkerThread::createThread(watchHangs, nullptr, 0x10000, 10, OS_THREAD_ATTRIB_AFFINITY_CPU2);
    WorkerThread::Entry entry = [](int argc, void *argv) -> int {
        runUi(argc, argv);
        std::unique_lock<std::mutex> lock(procMutex);
        uiDone = true;
        procCond.notify_all();

        // Shut down graphics on this core, where they were set up, once the app is exiting
        procCond.wait(lock, [] { return exitRequested; });
        lock.unlock();
        closeGraphics();
        lock.lock();
        uiClosed = true;
        procCond.notify_all();
        return 0;
    };

    if (WorkerThread::createThread(entry, nullptr, 0x100000,
            OSGetThreadPriority(OSGetCurrentThread()), OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        // Handle system messages while the UI runs, which ProcUI only allows on this core
        bool launched = false;
        while (true) {
            switch (ProcUIProcessMessages(true)) {
            case PROCUI_STATUS_EXITING:
                // Exit, leaving the UI stopped if it's still running; save data was written when it stopped
                // The network is stopped first, since the system waits on its thread and sockets otherwise
                ConsoleUI::stopNetwork();
                {
                    // Let a UI that finished shut down graphics first
                    std::unique_lock<std::mutex> lock(procMutex);
                    if (uiDone) {
                        exitRequested = true;
                        procCond.notify_all();
                        procCond.wait(lock, [] { return uiClosed; });
                    }
                }
                ProcUIShutdown();
                return 0;

            case PROCUI_STATUS_RELEASE_FOREGROUND: {
                // Wait for the UI to stop drawing before releasing the foreground
                std::unique_lock<std::mutex> lock(procMutex);
                releaseRequested = true;
                procCond.wait(lock, [] { return uiParked || uiDone; });
                lock.unlock();
                ProcUIDrawDoneRelease();
                break;
            }

            default: {
                // Let the UI continue if the foreground came back
                std::unique_lock<std::mutex> lock(procMutex);
                if (releaseRequested) {
                    releaseRequested = false;
                    procCond.notify_all();
                }

                // Return to the system menu once the UI has finished
                if (uiDone && !launched) {
                    launched = true;
                    lock.unlock();
                    SYSLaunchMenu();
                    break;
                }
                lock.unlock();
                OSSleepTicks(OSMillisecondsToTicks(20));
                break;
            }
            }
        }
    }

    runUi(0, nullptr);
    SYSLaunchMenu();

    // Respond to system messages appropriately to allow exiting
    while (true) {
        switch (ProcUIProcessMessages(true)) {
        case PROCUI_STATUS_EXITING:
            closeGraphics();
            ProcUIShutdown();
            return 0;

        case PROCUI_STATUS_RELEASE_FOREGROUND:
            ProcUIDrawDoneRelease();
            break;

        case PROCUI_STATUS_IN_FOREGROUND: case PROCUI_STATUS_IN_BACKGROUND:
            break;
        }
    }
}

