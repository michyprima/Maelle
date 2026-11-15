// Runs a PowerPC program built with devkitPPC in Unicorn, serving its calls for files, the time and exiting; game.sh
// uses it to run game.cpp and the core as the console's compiler builds them, generated code included
// Usage: host <program.elf> [arguments for it]
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <unicorn/unicorn.h>
#include "layout.h"

static uc_engine *uc;
static int exitCode = -1;
static uint64_t hostCalls;

// With GUEST_PROFILE=first:last:path, the PowerPC instructions run at each address from frame first to before last, as
// counted by the frame lines the guest writes, go to path as lines of address and count, with generated code as one
// line at the codegen area's start; each instruction is counted as it runs, from the first frame on only
static int profileFirst = -1, profileLast = -1, frameNow = 0;
static std::string profilePath;
static std::vector<uint64_t> profileCounts;
static uint64_t profileGenerated;
static uc_hook profileHook;
static bool profiling;

// With GUEST_CALLERS=address,..., the profile also counts where calls to those function addresses came from, by the
// link register at their first instruction, as lines of "caller address count" after a "callee" line for each
static std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint64_t>> profileCallers;

static void hookInstruction(uc_engine*, uint64_t address, uint32_t, void*) {
    uint64_t index = (address - GUEST_TEXT) >> 2;
    if (index < profileCounts.size()) {
        profileCounts[index]++;
        if (!profileCallers.empty()) {
            auto callee = profileCallers.find(address);
            if (callee != profileCallers.end()) {
                uint32_t lr = 0;
                uc_reg_read(uc, UC_PPC_REG_LR, &lr);
                callee->second[lr]++;
            }
        }
    }
    else if (address - GUEST_CODEGEN < GUEST_CODEGEN_SIZE) profileGenerated++;
}

static void writeProfile() {
    if (profilePath.empty()) return;
    FILE *file = fopen(profilePath.c_str(), "w");
    if (!file) return;
    for (size_t i = 0; i < profileCounts.size(); i++)
        if (profileCounts[i]) fprintf(file, "%08X %llu\n", uint32_t(GUEST_TEXT + i * 4), (unsigned long long)profileCounts[i]);
    fprintf(file, "%08X %llu\n", GUEST_CODEGEN, (unsigned long long)profileGenerated);
    for (auto &callee: profileCallers) {
        fprintf(file, "callee %08X\n", callee.first);
        for (auto &caller: callee.second)
            fprintf(file, "caller %08X %llu\n", caller.first, (unsigned long long)caller.second);
    }
    fclose(file);
}

static void updateProfile() {
    // Translated code only calls hooks that were there when it was translated, so start over after a change
    bool on = frameNow >= profileFirst && frameNow < profileLast;
    if (on == profiling) return;
    if (on) uc_hook_add(uc, &profileHook, UC_HOOK_CODE, (void*)hookInstruction, nullptr, 1, 0);
    else uc_hook_del(uc, profileHook);
    uc_ctl_flush_tb(uc);
    profiling = on;
}

static void fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    _exit(3);
}

static uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t reg(int r) { uint32_t v = 0; uc_reg_read(uc, r, &v); return v; }
static void setReg(int r, uint32_t value) { uc_reg_write(uc, r, &value); }

static std::string guestString(uint32_t address) {
    std::string s;
    char c;
    while (uc_mem_read(uc, address++, &c, 1) == UC_ERR_OK && c) s += c;
    return s;
}

static void hookCall(uc_engine*, uint64_t address, uint32_t, void*) {
    uint32_t call = (address - GUEST_HOST_CALLS) / 4;
    uint32_t a = reg(UC_PPC_REG_3), b = reg(UC_PPC_REG_4), c = reg(UC_PPC_REG_5);
    int32_t result = 0;
    static std::vector<uint8_t> buffer;
    hostCalls++;
    switch (call) {
        case GUEST_EXIT:
            exitCode = a;
            writeProfile();
            if (a == 134) {
                // An abort: the return addresses up the stack's back chain, for addr2line
                fprintf(stderr, "stack:");
                uint32_t sp = reg(UC_PPC_REG_1);
                for (int i = 0; i < 32 && sp; i++) {
                    uint8_t frame[8];
                    if (uc_mem_read(uc, sp, frame, 8) != UC_ERR_OK) break;
                    sp = be32(frame);
                    uint8_t lr[4];
                    if (!sp || uc_mem_read(uc, sp + 4, lr, 4) != UC_ERR_OK) break;
                    fprintf(stderr, " %08X", be32(lr));
                }
                fprintf(stderr, "\n");
            }
            uc_emu_stop(uc);
            return;
        case GUEST_WRITE:
            buffer.resize(c);
            uc_mem_read(uc, b, buffer.data(), c);
            result = write(a, buffer.data(), c);
            if (a == 1) {
                for (uint32_t i = 0; i + 6 <= c; i++)
                    if ((i == 0 || buffer[i - 1] == '\n') && !memcmp(&buffer[i], "frame ", 6)) frameNow++;
                if (!profilePath.empty()) updateProfile();
            }
            break;
        case GUEST_OPEN: {
            int flags = ((b & GUEST_O_WRONLY) ? O_WRONLY : (b & GUEST_O_RDWR) ? O_RDWR : O_RDONLY) |
                ((b & GUEST_O_CREAT) ? O_CREAT : 0) | ((b & GUEST_O_TRUNC) ? O_TRUNC : 0) |
                ((b & GUEST_O_APPEND) ? O_APPEND : 0) | ((b & GUEST_O_EXCL) ? O_EXCL : 0);
            result = open(guestString(a).c_str(), flags, 0644);
            break;
        }
        case GUEST_CLOSE: result = close(a); break;
        case GUEST_READ: {
            buffer.resize(c);
            ssize_t done = read(a, buffer.data(), c);
            if (done > 0) uc_mem_write(uc, b, buffer.data(), done);
            result = done;
            break;
        }
        case GUEST_SEEK: result = lseek(a, int32_t(b), c); break;
        case GUEST_SIZE: {
            struct stat st;
            result = fstat(a, &st) ? -1 : int32_t(st.st_size);
            break;
        }
        case GUEST_STAT: {
            struct stat st;
            result = stat(guestString(a).c_str(), &st) ? -1 : S_ISDIR(st.st_mode) ? -2 : int32_t(st.st_size);
            break;
        }
        case GUEST_UNLINK: result = unlink(guestString(a).c_str()); break;
        case GUEST_RENAME: result = rename(guestString(a).c_str(), guestString(b).c_str()); break;
        case GUEST_MKDIR: result = mkdir(guestString(a).c_str(), 0755); break;
        case GUEST_TRUNCATE: result = ftruncate(a, b); break;
        case GUEST_TIME: {
            uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            uint8_t words[8];
            for (int i = 0; i < 8; i++) words[i] = ns >> (56 - i * 8);
            uc_mem_write(uc, a, words, 8);
            break;
        }
        case GUEST_ZONE: {
            // TZ if it's a rule, otherwise the rule zone files end with, so local times are the Mac's
            std::string zone;
            const char *tz = getenv("TZ");
            if (tz && *tz && *tz != ':' && *tz != '/') {
                zone = tz;
            }
            else if (FILE *file = fopen((tz && *tz) ? (tz + (*tz == ':')) : "/etc/localtime", "rb")) {
                std::vector<char> data(64 * 1024);
                data.resize(fread(data.data(), 1, data.size(), file));
                fclose(file);
                if (data.size() > 2 && data.back() == '\n') {
                    size_t start = data.size() - 1;
                    while (start > 0 && data[start - 1] != '\n') start--;
                    zone.assign(&data[start], data.size() - 1 - start);
                }
            }
            zone.resize(std::min<size_t>(zone.size(), b - 1));
            uc_mem_write(uc, a, zone.c_str(), zone.size() + 1);
            break;
        }
        default: fatal("unknown host call %u", call);
    }
    setReg(UC_PPC_REG_3, result);
}

static bool hookBadMemory(uc_engine*, uc_mem_type type, uint64_t address, int size, int64_t, void*) {
    fatal("bad memory %s of %d bytes at %08llX, PC %08X, LR %08X", (type == UC_MEM_FETCH_UNMAPPED ||
        type == UC_MEM_FETCH_PROT) ? "fetch" : (type == UC_MEM_WRITE_UNMAPPED || type == UC_MEM_WRITE_PROT) ? "write" : "read",
        size, (unsigned long long)address, reg(UC_PPC_REG_PC), reg(UC_PPC_REG_LR));
    return false;
}

static void map(uint32_t start, uint32_t end, uint32_t perms, const char *what) {
    start &= ~0xFFF;
    end = (end + 0xFFF) & ~0xFFF;
    if (uc_mem_map(uc, start, end - start, perms) != UC_ERR_OK) fatal("couldn't map %s at %08X-%08X", what, start, end);
}

int main(int argc, char **argv) {
    if (argc < 2) fatal("usage: host <program.elf> [arguments]");
    FILE *file = fopen(argv[1], "rb");
    if (!file) fatal("couldn't open %s", argv[1]);
    std::vector<uint8_t> elf;
    fseek(file, 0, SEEK_END);
    elf.resize(ftell(file));
    fseek(file, 0, SEEK_SET);
    fread(elf.data(), 1, elf.size(), file);
    fclose(file);
    if (elf.size() < 52 || memcmp(elf.data(), "\x7F" "ELF\x01\x02", 6) || be16(&elf[18]) != 20)
        fatal("%s isn't a big-endian 32-bit PowerPC ELF", argv[1]);

    if (uc_open(UC_ARCH_PPC, uc_mode(UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN), &uc) != UC_ERR_OK) fatal("couldn't start Unicorn");
    uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_750_V3_1);

    // The program's segments, code that runs mapped executable and the rest not, since Unicorn watches executable pages
    // for code being written over, which slows every write to them
    uint32_t entry = be32(&elf[24]), phoff = be32(&elf[28]), phnum = be16(&elf[44]), end = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = &elf[phoff + i * 32];
        if (be32(ph) != 1) continue; // PT_LOAD
        uint32_t offset = be32(ph + 4), vaddr = be32(ph + 8), filesz = be32(ph + 16), memsz = be32(ph + 20), flags = be32(ph + 24);
        uint32_t perms = UC_PROT_READ | UC_PROT_WRITE | ((flags & 1) ? UC_PROT_EXEC : 0);
        // Segments can share a page; the later one's is mapped from the next page, with the shared page left as mapped
        uint32_t start = std::max(vaddr & ~0xFFF, end);
        if (start < vaddr + memsz) map(start, vaddr + memsz, perms, "a segment");
        uc_mem_write(uc, vaddr, &elf[offset], filesz);
        end = std::max(end, (vaddr + memsz + 0xFFF) & ~0xFFF);
    }
    if (end > GUEST_HEAP_END) fatal("the program reaches past the heap");
    map(end, GUEST_HEAP_END, UC_PROT_READ | UC_PROT_WRITE, "the heap");
    map(GUEST_CODEGEN, GUEST_CODEGEN + GUEST_CODEGEN_SIZE, UC_PROT_ALL, "the codegen area");
    map(GUEST_STACK, GUEST_ARGS + 0x10000, UC_PROT_READ | UC_PROT_WRITE, "the stack");
    map(GUEST_HOST_CALLS, GUEST_HOST_CALLS + GUEST_CALLS * 4, UC_PROT_ALL, "the host calls");
    std::vector<uint8_t> blr(GUEST_CALLS * 4);
    for (size_t i = 0; i < blr.size(); i += 4) memcpy(&blr[i], "\x4E\x80\x00\x20", 4);
    uc_mem_write(uc, GUEST_HOST_CALLS, blr.data(), blr.size());

    // The arguments, as pointers then the strings
    uint32_t args = GUEST_ARGS, strings = GUEST_ARGS + 4 * argc;
    for (int i = 1; i < argc; i++) {
        uint8_t word[4] = { uint8_t(strings >> 24), uint8_t(strings >> 16), uint8_t(strings >> 8), uint8_t(strings) };
        uc_mem_write(uc, args + (i - 1) * 4, word, 4);
        uc_mem_write(uc, strings, argv[i], strlen(argv[i]) + 1);
        strings += strlen(argv[i]) + 1;
    }
    if (strings > GUEST_ARGS + 0x10000) fatal("too many arguments");

    uc_hook calls, bad;
    uc_hook_add(uc, &calls, UC_HOOK_CODE, (void*)hookCall, nullptr, GUEST_HOST_CALLS, GUEST_HOST_CALLS + GUEST_CALLS * 4 - 1);
    uc_hook_add(uc, &bad, UC_HOOK_MEM_INVALID, (void*)hookBadMemory, nullptr, 1, 0);
    if (const char *profile = getenv("GUEST_PROFILE")) {
        char path[1024] = {};
        if (sscanf(profile, "%d:%d:%1023s", &profileFirst, &profileLast, path) != 3) fatal("GUEST_PROFILE is first:last:path");
        profilePath = path;
        profileCounts.resize((end - GUEST_TEXT) / 4);
        if (const char *callers = getenv("GUEST_CALLERS")) {
            for (const char *p = callers; *p;) {
                profileCallers[strtoul(p, nullptr, 16)];
                p = strchr(p, ',');
                if (!p) break;
                p++;
            }
        }
    }
    // The FPU starts off, as after a reset
    setReg(UC_PPC_REG_MSR, reg(UC_PPC_REG_MSR) | 0x2000);
    setReg(UC_PPC_REG_3, argc - 1);
    setReg(UC_PPC_REG_4, GUEST_ARGS);
    uc_err err = uc_emu_start(uc, entry, 0xFFFFFFFF, 0, 0);
    if (exitCode < 0) {
        fprintf(stderr, "registers:");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%s r%d %08X", (i % 8) ? "" : "\n", i, reg(UC_PPC_REG_0 + i));
        fatal("\nthe program stopped: %s at %08X, LR %08X", uc_strerror(err), reg(UC_PPC_REG_PC), reg(UC_PPC_REG_LR));
    }
    return exitCode;
}
