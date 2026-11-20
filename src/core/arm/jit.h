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

#include <cstdint>
#include <unordered_map>
#include <vector>

// How long each step of compiling a block takes, for builds made with JIT_SPLIT
#ifdef JIT_SPLIT
#include <coreinit/time.h>
enum JitSplitSlot {
    JS_TOTAL, // Compiling a block, including everything below and decoding it
    JS_NEW, // Allocating the block
    JS_REUSE, // Looking for a block to reuse code from, and generating an entry point into it
    JS_DECODE, // Decoding instructions
    JS_RETIRE, // Replacing a block already at this position
    JS_TRACK, // Tracking the block in its page, other than unmapping it
    JS_GENERATE, // Generating and finishing host code, including the steps below
    JS_PROTECT, // Unmapping pages that got their first code from the write maps
    JS_SWITCH_RW, // Making the codegen area writable
    JS_EMIT, // Generating host code
    JS_LINK, // Linking the block with others
    JS_FLUSH, // Flushing the code from the data cache
    JS_SWITCH_RX, // Making the codegen area executable again
    JS_INVALIDATE, // Invalidating the instruction cache
    JS_COUNT
};
extern uint64_t jitSplit[JS_COUNT];
extern uint32_t jitSplitBlocks, jitSplitBytes;
#define JS_START(name) uint32_t name = OSGetSystemTick()
#define JS_ADD(slot, name) jitSplit[slot] += uint32_t(OSGetSystemTick() - name)
#else
#define JS_START(name)
#define JS_ADD(slot, name)
#endif

class Core;
class Interpreter;
class PpcEmitter;

// Diagnostic logging for builds made with JIT_LOG, written to the SD card a line at a time
#ifdef JIT_LOG
void jitLog(const char *format, ...);
#define JLOG(...) jitLog(__VA_ARGS__)
#else
#define JLOG(...) do {} while (0)
#endif

// A decoded instruction, with the interpreter handler that executes it
struct JitInstr {
    uint32_t opcode;
    union {
        int (Interpreter::*arm)(uint32_t);
        int (Interpreter::*thumb)(uint16_t);
    };
};

struct JitBlock;

// A jump in generated code to a block at a known address, which goes through the dispatcher until it's
// patched to jump to the block's code directly
struct JitLink {
    uint32_t *site;
    uint32_t key; // Target address, with bit 0 set for THUMB
    JitBlock *target;
    bool sameLine; // Whether the jump is in the same line of code as its target, which the code cache then holds
};

// Lines of code that weren't all in the ARM9's code cache when a block being generated checked, to fetch after its code
struct JitFetch {
    uint32_t *site; // The branch taken when a line is missing
    uint32_t line; // The first line's place in main RAM
    int lines;
    uint32_t pc; // The program counter register's value at the instruction, or 0 if it's set and only behind
    uint32_t behind;
};

// A call in a block being generated to the routine for a list of registers that a block load or store moves
struct JitListCall {
    uint32_t *site;
    uint32_t key; // The list without the program counter, then bits for the user's registers and for a load
    bool offset; // Whether the address is already worked out, and not just the base
};

// A run of consecutive instructions from one 4KB page of host memory
struct JitBlock {
    uint32_t address; // Guest address of the first instruction
    bool thumb;
    uint8_t *start, *end; // Host memory covered by the block
    int index; // Position in the block lookup table
    std::vector<JitInstr> instrs;
    void *code = nullptr; // Generated host code, or null to use the portable executor
    std::vector<uint32_t> entries; // Offsets in the code where each instruction starts
    std::vector<uint8_t> entriesBehind; // How far the program counter register is behind at each of those
    std::vector<uint16_t> entriesCycles; // Cycles of the run each of those is in that are counted before it
    std::vector<JitLink> links; // Jumps from this block's code to other blocks
    std::vector<std::pair<JitBlock*, uint32_t*>> incoming; // Linked jumps to this block, and their blocks
    int linkedPos = -1; // Position in the list of blocks with linked jumps, or -1 if it's not there
    uint8_t afterFetch = 0; // Offset in the code past a check that the code cache holds only the first line, or 0
    bool exact = false; // Whether it checks whether to stop after every instruction, even with fast timing

    static void *operator new(size_t size);
    static void operator delete(void *block);
};

// A direct-mapped cache of recently run blocks by guest address, for jumping between blocks quickly
// Entries are 16 bytes on 32-bit hosts, so generated code can index them with a shift
struct alignas(16) JitCacheEntry {
    uint32_t key; // Guest address, with bit 0 set for THUMB
    void *code;
    JitBlock *block;
};

// Per-page list of compiled blocks, with how many blocks cover each halfword, so retiring a block only has to
// update its own range; counts stop at 255, and a page that reached that is recounted when blocks are retired
struct JitPage {
    std::vector<JitBlock*> blocks;
    uint8_t covered[0x1000 / 2] = {};
    bool saturated = false;
    uint16_t overwrites = 0; // Writes over its code since the page last got code
    uint8_t cleared = 0; // Times its code was dropped for being overwritten, which makes that take more writes
};

// Host memory that code is compiled from, and whether code there can be overwritten
struct JitRegion {
    uint8_t *base;
    uint32_t size;
    bool writable;
};

// Runs one CPU's code in compiled blocks, matching the interpreter instruction for instruction
// Blocks remove the per-instruction fetch and dispatch overhead, and generate native code where possible
class Jit {
public:
    friend bool jitTestEmit(Jit &jit, uint8_t *buf, uint32_t op, uint32_t pc, uint32_t *size, uint32_t *start);
    bool enabled = false;
    static bool testing; // Set by the self-test, so compiled code always checks every instruction like the interpreter

    Jit(Core *core, bool arm7);
    ~Jit();

    void enable(bool value);
    void reset();
    bool run(const uint32_t *limit);

    bool isNative() { return codeArea != nullptr; }
    bool hasCode() { return codePages != 0; }
    bool isCodePage(uint8_t *host);
    bool lineHasCode(uint8_t *host);
    bool writeCode(uint8_t *host, int size);
    void mapChanged(uint32_t start);
    void setHalfCycles(bool value) { nextHalfCycles = value && !arm7; }

private:
    Core *core;
    bool arm7;

    // Blocks are looked up by the host address of their first halfword
    // The ARM9 compiles code from main RAM, ITCM and its BIOS; the ARM7 from main RAM, both WRAMs and its BIOS
    // Pages of the writable regions are tracked, which is 4MB of main RAM or 16MB in DSi mode, then the rest
    JitRegion regions[6] = {};
    int regionCount = 0;
    uint32_t tableSize = 0;
    std::vector<JitBlock*> table;
    std::vector<JitPage> pages;
    int codePages = 0;
    bool halfCycles = false; // Whether this is the DSi's ARM9 at 133MHz, which counts half cycles
    bool nextHalfCycles = false; // The speed to count at once no code is running, since code calls stubs for one speed
    bool codeFullSpeed = false; // Whether the ARM7's code was generated with its WRAM at full speed

    // The entry before the cache holds a flag that's set when code changes, so it's next to the cache
    // Running code stops at the next instruction when the flag is set, in case its own code changed
    // Indexed by the address over 2, so THUMB blocks a halfword apart, common in ARM7 code, don't share an entry; ARM code
    // only fills even entries, so it gets half of them
    static const int CACHE_BITS = 16, CACHE_SIZE = 1 << CACHE_BITS;
    JitCacheEntry cacheData[CACHE_SIZE + 1] = {};
    static const uint32_t NO_KEY = 2; // A key no code has
    static uint32_t cacheKey(uint32_t address, bool thumb) { return address + (thumb ? 3 : 4); } // The program counter there, with bit 0 for THUMB
    JitCacheEntry &cacheEntry(uint32_t key) { return cache[(key >> 1) & (CACHE_SIZE - 1)]; }
    JitCacheEntry *cache = &cacheData[1];
    uint32_t &codeChanged = cacheData[0].key;
    // Entries that blocks from WRAM addresses were cached in, which the DSi's launcher remaps thousands of times, so
    // only they are emptied then; past the limit, which a DS game's ARM7 code in WRAM reaches, the whole cache is
    static const size_t MAX_WRAM_ENTRIES = 0x400;
    std::vector<uint32_t> wramEntries;
    bool wramFull = false;
    static const int NO_CODE_SIZE = 16;
    uint32_t noCodePages[NO_CODE_SIZE] = {}; // Guest pages recently found to have nothing to compile, plus 1, by low bits

    // Links waiting for their target to be compiled, and jumps to put back to the dispatcher before running again
    std::unordered_multimap<uint32_t, std::pair<JitBlock*, uint32_t*>> pendingLinks;
    std::vector<uint32_t*> unlinks;

    // Blocks that may have jumps linked to them, so a change to the memory map doesn't have to search the whole table
    std::vector<JitBlock*> linkedBlocks;

    // Blocks can be invalidated while running, so they're deleted later
    std::vector<JitBlock*> graveyard;
    std::vector<JitBlock*> hitBlocks; // Blocks a write overlaps, kept to avoid allocating for every write
    bool running = false;

    // Generated code, starting with shared entry, exit, dispatch and per-instruction routines
    uint8_t *codeArea = nullptr, *codeStart = nullptr, *codePtr = nullptr, *codeEnd = nullptr;
    void *codeEnter = nullptr, *codeExit = nullptr, *codeDispatch = nullptr;
    void *codeRefresh = nullptr, *codeDispatchMiss = nullptr;
    uint32_t runCycles = 0; // Cycles of the run being generated so far that its check adds as a constant
    uint32_t pcBehind = 0; // How far the program counter register will be behind at this point in a block being generated
    std::vector<uint8_t> liveFlags; // Which of N, Z, C and V are read after each instruction of the block being generated
    uint8_t flagsLive = 0xF; // The same for the instruction being generated, as bits 28 to 31 of the CPSR
    bool thumbCode = false; // Whether the block being generated is THUMB
    bool fullSpeed = false; // Whether it's the ARM7's BIOS, or DSi code outside main RAM, whose cycles aren't doubled
    void *codePost[3][2] = {}, *codeSlowPost[2] = {}; // Indexed by how cycles are halved, then storing the PC
    void *codeFetch[2] = {}; // Fetching a line into the ARM9's code cache, with a second for when it might be there already
    std::vector<JitFetch> fetchPaths; // Fetches to generate after the code being generated
    std::vector<JitListCall> listCalls; // Calls to point at their routines after the code being generated
    std::vector<uint32_t*> patchedSites; // Jumps patched when linking the code being generated, kept to save allocating
    std::unordered_map<uint32_t, void*> listStubs; // Routines for block loads and stores made since the area started over
    void *codeCond[7] = {}, *codeFlags[5] = {}, *codeMemory[6] = {}, *codeShift[4] = {}, *codeShiftCarry[4] = {}, *codeMultiplyCycles = nullptr, *codeMultiplyCyclesUnsigned = nullptr, *codeOverflowQ = nullptr, *codeMsr[2] = {}, *codeReturn = nullptr;
    int offPc = 0, offCpsr = 0, offSpsr = 0, offCycles = 0, offEventCycles = 0, offUsr = 0, offRegs = 0, offIe = 0, offIrf = 0;
    int offDsiCycle = 0, offIme = 0;
    uint32_t codeCore = 0;
    bool codeFailed = false;
    bool codeFast = false; // Whether code was generated for fast timing, which checks cycles less often
    bool half(const JitBlock *block); // Whether a block's cycles count half
    int postHalf(const JitBlock *block) { return arm7 ? fullSpeed : !half(block) ? 0 : (codeFast && !testing) ? 2 : 1; } // Which post-instruction stubs to use

    // Addresses that native instructions use, which tests can point elsewhere
    uint32_t addrMemory = 0, addrReadMap = 0, addrWriteMap = 0;
    uint32_t addrDtcmReg = 0, addrWriteCpsr = 0, addrRestoreCpsr = 0, addrModeBanks = 0;
    uint32_t addrDivCnt = 0, addrDivNumer = 0, addrDivDirty = 0, addrSqrtCnt = 0, addrSqrtParam = 0, addrSqrtDirty = 0, addrGxStat = 0;
    uint32_t addrLoad32 = 0, addrLoad8 = 0, addrStore32 = 0, addrStore8 = 0, addrLoad16 = 0, addrStore16 = 0;
    uint32_t addrCodeLines = 0, addrCodeTags = 0, addrCodeTurn = 0;

    uint32_t modeBanks[32][4] = {}; // R13, R14 and the SPSR by CPU mode, for returning from exceptions in generated code

    uint8_t **codeMap();
    int tableIndex(uint8_t *host);
    int pageIndex(uint8_t *host);
    uint8_t *pageHost(int page);
    JitBlock *compile(uint32_t address, bool thumb, uint8_t *host, int index);
    void retire(JitBlock *block);
    JitBlock *lookup(uint32_t address, bool thumb);
    void addLinked(JitBlock *block);
    void unlinkIncoming(JitBlock *block);
    void removeLinks(JitBlock *block);
    void linkBlock(JitBlock *block, std::vector<uint32_t*> &patched);
    void applyUnlinks();
    void patchJump(uint32_t *site, void *target);
    void protect(int page);
    void execute(JitBlock *block, const uint32_t *limit);
    int interleave(const uint32_t *limit);
    static int interleaveCall(Jit *jit, const uint32_t *limit);
    bool pollsGxStat(JitBlock *block, size_t index);
    uint32_t skipPoll(uint32_t load, uint32_t test, uint32_t branch, uint32_t cycles, uint32_t next);
    static uint32_t skipPollCall(Jit *jit, uint32_t load, uint32_t test, uint32_t branch, uint32_t cycles, uint32_t next);
    uint32_t sendsWords(JitBlock *block, size_t index);
    uint32_t refreshNext(const uint32_t *limit);
    int sendWords(uint32_t address, uint32_t regs, const uint32_t *limit, uint32_t cycles, uint32_t next);
    static int sendWordsCall(Jit *jit, uint32_t address, uint32_t regs, const uint32_t *limit, uint32_t cycles, uint32_t next);
    static void writeCpsrCall(Interpreter *arm, uint32_t opcode, uint32_t value);
    static void restoreCpsrCall(Interpreter *arm, uint32_t before);

    static bool endsBlock(uint32_t opcode, bool thumb);

    void initCodegen();
    void freeCodegen();
    bool codeFull();
    void finishCode(uint8_t *start, size_t size, const std::vector<uint32_t*> &patched);
    void *generate(JitBlock *block);
    void *generateEntry(JitBlock *block, JitBlock *host, size_t index);
    void linkEntry(JitBlock *block);
    void emitStubs(PpcEmitter &e);
#if defined(PROFILE_EMU) && defined(HEARTBEAT)
    void traceStubs();
#endif
    void emitMath(PpcEmitter &e, bool load, int size);
    bool emitNative(PpcEmitter &e, uint32_t op, uint32_t pc);
    uint32_t *condSkip(PpcEmitter &e, uint32_t cond);
    uint32_t armOpcode(JitBlock *block, size_t i);
    void findLiveFlags(JitBlock *block);
    bool batched(JitBlock *block, size_t i);
    bool fetches(JitBlock *block);
    int fetchLines(JitBlock *block, size_t i, bool entry, uint32_t &address);
    void emitFetch(PpcEmitter &e, uint32_t address, int lines, uint32_t pc);
    void emitFetchPaths(PpcEmitter &e);
    void emitListStubs(PpcEmitter &e, bool keep);
    bool endsRun(JitBlock *block, size_t i);
    void stepPc(PpcEmitter &e);
    void emitInstr(PpcEmitter &e, JitBlock *block, size_t i, uint32_t pc, bool &ok);
    void emitAlu(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBranch(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBlx(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitThumbBranch(PpcEmitter &e, uint32_t op, uint32_t pc, bool known = false, uint32_t target = 0);
    void emitTransfer(PpcEmitter &e, uint32_t op, uint32_t pc);
    bool emitShift(PpcEmitter &e, uint32_t op, uint32_t pc, bool setCarry);
    void emitHalf(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBlock(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitSwap(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitMultiply(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitDsp(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitMisc(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitCoprocessor(PpcEmitter &e, uint32_t op);
    void emitMsr(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitBranchReg(PpcEmitter &e, uint32_t op, uint32_t pc);
    void emitJump(PpcEmitter &e, int rs, bool interwork);
    void emitPollSkip(PpcEmitter &e, JitBlock *block, size_t index);
    void emitSendWords(PpcEmitter &e, JitBlock *block, size_t index);
    void readReg(PpcEmitter &e, int rt, int reg, uint32_t pc);
    void writeReg(PpcEmitter &e, int rs, int reg);
    void clearCache();
    void clearWramCache();
};
