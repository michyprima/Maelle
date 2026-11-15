#!/usr/bin/env python3
# Changes a copy of the core so it builds on a Mac, with generated code going to the test instead of being run
# Usage: patch.py <copy of src/core> [mutation number] [game], where a mutation plants a bug the tests should catch
# With game, generated code runs whole games through game.cpp instead of being checked a case at a time
# With plant, only the bug is planted, in a copy of the core that's otherwise left as it is, for game.sh's gcc- modes
import os, re, sys

root = sys.argv[1]
mutation = int(sys.argv[2]) if len(sys.argv) > 2 else 0
game = len(sys.argv) > 3 and sys.argv[3] == 'game'
plant = len(sys.argv) > 3 and sys.argv[3] == 'plant'

def edit(path, *changes):
    path = root + '/' + path
    text = open(path).read()
    for old, new in changes:
        assert old in text, old
        text = text.replace(old, new)
    open(path, 'w').write(text)

# The changes for the Mac are left out when planting, and only the bug is made, with change
change = edit
if plant:
    edit = lambda path, *changes: None

edit('arm/interpreter.h', ('private:', 'public:'))

# Older cores, which bench.sh compares with, don't have what this harness points elsewhere for returns and the math registers
if 'addrRestoreCpsr' not in open(root + '/arm/jit.h').read():
    edit('arm/jit.h', ('    uint32_t addrDtcmReg = 0, addrWriteCpsr = 0;\n', '    uint32_t addrDtcmReg = 0, addrWriteCpsr = 0, addrRestoreCpsr = 0, addrModeBanks = 0;\n'
        '    uint32_t addrDivCnt = 0, addrDivNumer = 0, addrDivDirty = 0, addrSqrtCnt = 0, addrSqrtParam = 0, addrSqrtDirty = 0;\n'
        '    static void restoreCpsrCall(Interpreter *arm, uint32_t before) {}\n    uint32_t modeBanks[32][4] = {};\n'))
if 'addrGxStat' not in open(root + '/arm/jit.h').read():
    edit('arm/jit.h', ('    uint32_t addrDtcmReg = 0,', '    int offIme = 0;\n    uint32_t addrGxStat = 0;\n    uint32_t addrDtcmReg = 0,'))
# and they don't count the time the ARM9's code cache takes
if 'addrCodeLines' not in open(root + '/arm/jit.h').read():
    edit('arm/jit.h', ('    uint32_t addrDtcmReg = 0,', '    uint32_t addrCp15 = 0, addrCodeLines = 0, addrCodeMiss = 0;\n    uint32_t addrDtcmReg = 0,'))
    edit('arm/cp15.h', ('    Cp15(Core *core): core(core) {}\n', '    Cp15(Core *core): core(core) {}\n    uint8_t codeLines[1] = {};\n    int codeMiss(uint32_t address) { return 0; }\n'))
    edit('arm/cp15.h', ('    uint8_t codeLines[1] = {};\n', '    uint8_t codeLines[1] = {}, codeTags[4] = {}, codeTurn = 0;\n'))
# Cores up to v1.0 fetch a line of code with a call, and keep where it goes to themselves
if 'addrCodeTags' not in open(root + '/arm/jit.h').read():
    edit('arm/jit.h', ('    uint32_t addrDtcmReg = 0,', '    uint32_t addrCodeTags = 0, addrCodeTurn = 0;\n    uint32_t addrDtcmReg = 0,'))
    edit('arm/cp15.h', ('private:\n    Core *core;\n', 'public:\n    Core *core;\n'))
# while newer ones fetch it in generated code, with no call
if 'addrCodeMiss' not in open(root + '/arm/jit.h').read():
    edit('arm/jit.h', ('    uint32_t addrDtcmReg = 0,', '    uint32_t addrCp15 = 0, addrCodeMiss = 0;\n    uint32_t addrDtcmReg = 0,'))
# Older cores also lack the DSi's headphone pin, reset request and SD/MMC write-back, which the harness uses
if not os.path.exists(root + '/io/dsi_gpio.h'):
    edit('core.h', ('#include "io/spi.h"\n', '#include "io/spi.h"\n\nstruct DsiGpio { static inline std::atomic<bool> headphonesIn{false}; };\n'))
if 'void flush();' not in open(root + '/memory/sd_mmc.h').read():
    edit('memory/sd_mmc.h', ('    void writeBlock();\n', '    void writeBlock();\n    void flush() {}\n'))
if 'restartRequested' not in open(root + '/core.h').read():
    edit('core.h', ('    Atomic32<bool> running;\n', '    Atomic32<bool> running;\n    std::atomic<bool> restartRequested{false};\n'))
# Cores up to v1.0 also look conditions up in the interpreter's table, which game.cpp gives them; newer ones don't have it
if 'addrCondition' not in open(root + '/arm/jit.h').read():
    edit('arm/jit.h', ('    uint32_t addrDtcmReg = 0,', '    uint32_t addrCondition = 0;\n    uint32_t addrDtcmReg = 0,'))
edit('arm/jit.cpp', ('#include <cstdlib>', '#include <algorithm>\n#include <vector>\n#include <cstdlib>'))
edit('defines.h', ('__asm__ volatile("dcbz 0, %0" :: "r"(data) : "memory");', ''))
edit('gpu/gpu.cpp',
    ('malloc_usable_size', 'malloc_size'), ('__os_snprintf', 'snprintf'),
    ('(uint32_t)drawer->getFramebuffer()', '(uint32_t)(uintptr_t)drawer->getFramebuffer()'),
    ('(uint32_t)fastTargets[engine]', '(uint32_t)(uintptr_t)fastTargets[engine]'), ('(uint32_t)drawer,', '(uint32_t)(uintptr_t)drawer,'))
if not plant:
    text = open(root + '/gpu/gpu.cpp').read()
    open(root + '/gpu/gpu.cpp', 'w').write('#include <malloc/malloc.h>\n' + text)
edit('gpu/gpu_3d_renderer.cpp', ('asm volatile("dcbz 0, %0" :: "r"(block) : "memory");', ';'))

# Older cores shift by 32 in this loop before checking m, which Clang assumes never happens, so it would give different
# timing to GCC's build; newer ones check m first
if '(op2 < (-1 << (m << 3)) || op2 >= (1 << (m << 3))) && m < 4' in open(root + '/arm/interpreter_alu.cpp').read():
    edit('arm/interpreter_alu.cpp', ('(op2 < (-1 << (m << 3)) || op2 >= (1 << (m << 3))) && m < 4',
        'm < 4 && (op2 < (-1 << (m << 3)) || op2 >= (1 << (m << 3)))'), ('(op3 < (-1 << (m << 3)) || op3 >= (1 << (m << 3))) && m < 4',
        'm < 4 && (op3 < (-1 << (m << 3)) || op3 >= (1 << (m << 3)))'))

# C++ functions called from generated code become fixed addresses that the test watches
path = root + '/arm/jit_ppc.cpp'
text = open(path).read()
text = re.sub(r'static_assert\(sizeof\(JitCacheEntry\) == 16[^\n]*\n', '', text)
text = text.replace('e.call((void*)&Jit::interleaveCall);', 'e.call((void*)(uintptr_t)0x3F2000);')
text = text.replace('e.call((void*)&Jit::skipPollCall);', 'e.call((void*)(uintptr_t)0x3F2008);')
text = text.replace('    emitStubs(e);\n    codeStart =', '    rigOverride(*this);\n    emitStubs(e);\n    codeStart =')
text = text.replace('#include "../core.h"', '#define private public\n#include "../core.h"\n#undef private\nvoid rigOverride(Jit &jit);', 1)

# Flags the JIT works out that nothing reads can be forced to 0 or to 1 where it stops keeping them, so a flag that's
# wrongly left out shows as a wrong value whatever the old one was; the test also counts what's left out
text = text.replace('void rigOverride(Jit &jit);', 'void rigOverride(Jit &jit);\nextern int rigPoison;\nextern uint64_t rigFlagStats[2];', 1)
# Older cores, which bench.sh compares with, work out every flag
if 'liveFlags' in text:
    old = '    flagsLive = liveFlags[i];\n'
    assert old in text, old
    text = text.replace(old, old + '    if (rigPoison && i > 0 && liveFlags[i - 1] != FLAG_ALL) {\n        e.lwz(6, offCpsr, R_ARM);\n'
        '        e.oris(6, 6, (FLAG_ALL & ~liveFlags[i - 1]) << 12);\n        if (rigPoison == 1) e.xoris(6, 6, (FLAG_ALL & ~liveFlags[i - 1]) << 12);\n'
        '        e.stw(6, offCpsr, R_ARM);\n    }\n')
    old = '    if (!need) s = false;\n'
    assert old in text, old
    text = text.replace(old, '    if (s && !need) rigFlagStats[0]++;\n    else if (!logical && need == FLAG_C) rigFlagStats[1]++;\n' + old)

if game:
    # Each handler gets its own address so the game run knows which to call, and code goes to the emulator when it's
    # flushed and runs there
    text = text.replace('return (parts.ptr & 1 || parts.adj) ? nullptr : (void*)parts.ptr;',
        'return (void*)(uintptr_t)rigHandler(&func, sizeof(func), std::is_same<T, int (Interpreter::*)(uint16_t)>::value);')
    text = text.replace('    JS_START(flushStart);\n    if (start) DCFlushRange', '    rigCodeWritten(*this, start, size, patched);\n'
        '    JS_START(flushStart);\n    if (start) DCFlushRange')
    text = text.replace('#include <cstring>', '#include <cstring>\n#include <type_traits>', 1)
    text = text.replace('void rigOverride(Jit &jit);', 'void rigOverride(Jit &jit);\nuint32_t rigHandler(const void *func, size_t size, bool thumb);\n'
        'void rigCodeWritten(Jit &jit, uint8_t *start, size_t size, const std::vector<uint32_t*> &patched);', 1)
    edit('arm/jit.cpp', ('        ((void(*)(Interpreter*, EventQueue*, const uint32_t*, void*, uint32_t*, JitCacheEntry*))\n'
        '            codeEnter)(&arm, &core->events, limit, block->code, &core->globalCycles, cache);',
        '        rigEnter(*this, limit, block->code);'),
        ('#include <algorithm>\n', '#include <algorithm>\nclass Jit;\nvoid rigEnter(Jit &jit, const uint32_t *limit, void *code);\n'))

    # The console's build sets a VRAM mapping's count before its pointer, which matters to a 2D thread reading it
    edit('memory/memory.cpp', ('    mappings[count++] = mapping;', '    uint8_t next = count;\n    *(volatile uint8_t*)&count = next + 1;\n'
        '    *(uint8_t *volatile*)&mappings[next] = mapping;'))
else:
    text = text.replace('return (parts.ptr & 1 || parts.adj) ? nullptr : (void*)parts.ptr;', 'return (void*)(uintptr_t)0x3F1000;')

# Bugs that should be caught, by which suite they're aimed at
MUTATIONS = [
    ('e.addi(8, 8, thumbCode ? 2 : 4);', 'e.addi(8, 8, 4);'), # 1: POP PC on the ARM7
    ('wordPc ? (pc & ~0x3) : pc', 'pc'), # 2: LDR from the PC isn't aligned
    ('if (thumbCode) e.rlwinm(6, 6, 0, 27, 25);\n', ''), # 3: BX doesn't go back to ARM
    ('(!thumbCode && !(op & BIT(25))', '(!(op & BIT(25))'), # 4: THUMB register shifts take 2 cycles
    ('case 0xE: return 0xE1D00000', 'case 0xE: return 0xE1900000'), # 5: BIC is ORR
    ('case 0x9: return 0xE2700000 | (rs << 16) | (rd << 12);', 'case 0x9: return 0xE2600000 | (rs << 16) | (rd << 12);'), # 6: NEG sets no flags
    ('        e.call(codeRefresh);\n        e.lwz(0, 20, 1);\n        e.mtlr(0);\n        e.addi(1, 1, 16);\n        e.blr();\n    }\n}', '        e.lwz(0, 20, 1);\n        e.mtlr(0);\n        e.addi(1, 1, 16);\n        e.blr();\n    }\n}'), # 7: memory slow path doesn't refresh
    ('        if (load) e.mr(8, 3);\n', ''), # 8: memory slow path loses a loaded value
    ('e.loadConst(R_PC, pc - host->entriesBehind[index]);', 'e.loadConst(R_PC, pc);'), # 9: mid-block entry has the wrong PC
    ('block->links.push_back({ e.here(), target | thumbCond, nullptr, sameLine(target) });', 'block->links.push_back({ e.here(), target, nullptr, sameLine(target) });'), # 10: THUMB branch links to ARM
    ('    e.stw(3, offCycles, R_ARM);\n    e.stw(R_CYCLES, 0, R_GLOBAL);\n', '    e.stw(3, offCycles, R_ARM);\n'), # 11: a stop doesn't save the time
    ('    stepPc(e);\n    e.call(codePost[postHalf(block)][!memoryPc]);', '    e.call(codePost[postHalf(block)][!memoryPc]);'), # 12: the PC is stale at a check
    ('*last = (14 << 26) | (3 << 21) | (R_BATCH << 16) | ((*last & 0xFFFF) + runCycles);', '*last = (14 << 26) | (3 << 21) | (R_BATCH << 16) | (*last & 0xFFFF);'), # 13: a run's cycles are lost
    ('    e.lwz(R_CYCLES, 0, R_GLOBAL);\n    e.lwz(4, 0, R_LIMIT);', '    e.lwz(4, 0, R_LIMIT);'), # 14: refresh doesn't reload the time
    ('e.rlwinm(R_TMP2, 5, 0, 0, 29);', 'e.mr(R_TMP2, 5);'), # 15: LDM/STM addresses aren't aligned
    ('case 0x7: // RSC\n            if (arith) e.subfe(5, 3, 4);', 'case 0x7: // RSC\n            if (arith) e.subfe(5, 4, 3);'), # 16: RSCS has its operands swapped
    ('e.call(codeFlags[(opc == 0x4 || opc == 0x5 || opc == 0xB) ? 2 : (opc == 0x3 || opc == 0x7) ? 4 : 3]);', 'e.call(codeFlags[(opc == 0x4 || opc == 0xB) ? 2 : (opc == 0x3 || opc == 0x7) ? 4 : 3]);'), # 17: ADCS has SBCS flags
    ('if (i + 1 == block->instrs.size()) e.stw(R_PC, offPc, R_ARM);\n        return;', 'return;'), # 18: a block ending in a conditional branch loses the PC
    ('block->links.push_back({ e.here(), target | 1, nullptr, sameLine(target) });', 'block->links.push_back({ e.here(), target, nullptr, sameLine(target) });'), # 19: THUMB B links to ARM
    ('uint32_t *last = e.here() - 1;\n        if ((*last & 0xFFFF0000) == 0x38600000 && (*last & 0xFFFF) > 0) {\n            *last = (14 << 26) | (R_BATCH << 21) | (R_BATCH << 16) | ((*last & 0xFFFF) - 1);', 'uint32_t *last = e.here() - 1;\n        if ((*last & 0xFFFF0000) == 0x38600000 && (*last & 0xFFFF) > 0) {\n            *last = (14 << 26) | (R_BATCH << 21) | (R_BATCH << 16) | (*last & 0xFFFF);'), # 20: folded conditions count a cycle too many
    ('e.call((!block->thumb && cond == 0xF) ? reserved : handler);\n        e.call(codeRefresh);\n', 'e.call((!block->thumb && cond == 0xF) ? reserved : handler);\n'), # 21: handlers don't refresh
    ('        e.stw(R_CYCLES, 0, R_GLOBAL);\n        e.stw(R_CYCLES, offCycles, R_ARM);\n        e.mr(3, R_ARM);\n', '        e.mr(3, R_ARM);\n'), # 22: handlers don't see the time
    ('    else if (block->thumb && (raw & 0xF800) == 0xE000) {\n        // Unconditional branches jump straight to their target like ARM ones\n        uint32_t target = pc + ((int32_t)(raw << 21) >> 20);\n        block->links.push_back({ e.here(), target | 1, nullptr, sameLine(target) });\n        e.b(codeDispatch);\n    }\n    else if', '    else if'), # 23: THUMB B doesn't link
    ('            e.rlwinm(5, 5, 0, 24, 31);\n            e.patchBc(store2);', '            e.patchBc(store2);'), # 24: MRS of a missing SPSR keeps the flags on the ARM9
    (' && reg != 0x070004 && reg != 0x070802;', ';'), # 25: MCR that halts has no effect
    ('e.rlwinm(6, 6, 0, 8, 31);\n        e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);\n    }\n    e.stw(6, 0, 5);', 'e.rlwimi(6, 4, 0, 0, arm7 ? 3 : 4);\n    }\n    e.stw(6, 0, 5);'), # 26: MSR SPSR keeps bits that don't exist
    ('e.andi_(8, 8, 0x3F);', 'e.andi_(8, 8, 0x1F);'), # 27: MSR setting THUMB stays out of the interpreter
    ('e.and_(9, 9, 10);\n        e.or_(8, 8, 9);', 'e.and_(9, 9, 10);'), # 28: MSR doesn't see all interrupts
    ('e.cmpwi(7, 0x10);\n        uint32_t *user', 'e.cmpwi(7, 0x11);\n        uint32_t *user'), # 29: User mode MSR changes control bits
    ('    if (load) {\n        if (writeback) writeReg(e, 9, rn);\n', '    if (load) {\n'), # 30: Halfword loads lose writeback
    ('blxTarget(op, pc) | 1, nullptr', 'blxTarget(op, pc), nullptr'), # 31: BLX links to ARM
    ('e.li(3, 5);\n        return;', 'e.li(3, 3);\n        return;'), # 32: LDR into the PC is too quick
    ('+ (jump ? 2 : 0)', ''), # 33: ALU writes to the PC are too quick
    ('e.rlwinm(8, 8, 28, 4, 4);', 'e.li(8, 0);'), # 34: SMLA doesn't set Q
    ('e.addc(5, 5, 7);\n        e.adde(6, 6, 8);\n        writeReg(e, 5, rn);', 'e.add(5, 5, 7);\n        e.adde(6, 6, 8);\n        writeReg(e, 5, rn);'), # 35: SMLAL loses the carry
    ('e.srawi(5, 3, 31);\n        e.loadConst(6, 0x7FFFFFFF);', 'e.srawi(5, 4, 31);\n        e.loadConst(6, 0x7FFFFFFF);'), # 36: QSUB saturates the wrong way
    ('e.andi_(7, 6, BIT(5));\n        uint32_t *arm', 'e.andi_(7, 7, BIT(5));\n        uint32_t *arm'), # 37: A return uses the old THUMB state
    ('e.or_(8, 8, 9);\n        e.cmpwi(8, 0);\n        uint32_t *slow2 = e.here();\n        e.bne(nullptr);\n        e.patchBc(masked);\n\n        // Swap', 'e.cmpwi(8, 0);\n        uint32_t *slow2 = e.here();\n        e.bne(nullptr);\n        e.patchBc(masked);\n\n        // Swap'), # 38: A return doesn't see all interrupts
    ('    if (restore) s = false;\n', ''), # 39: A return sets flags, which shows without an SPSR
    ('e.cmpwi(8, 0x11);\n        uint32_t *slow3', 'e.cmpwi(8, 0x12);\n        uint32_t *slow3'), # 40: A return from FIQ stays out of the interpreter
    ('                    e.lwbrx(8, 0, 6);\n                    if (--left) e.addi(6, 6, 4);\n                    if (user) e.stw(8, offUsr + i * 4, R_ARM);\n                    else writeReg(e, 8, i);', '                    e.lwbrx(8, 0, 6);\n                    if (--left) e.addi(6, 6, 4);\n                    writeReg(e, 8, i);'), # 41: LDM with S loads the banked registers
    ('            e.xori(6, 6, 4);\n', ''), # 42: Math registers have their words swapped
    ('banks(0x17, &arm.registersAbt[0], &arm.registersAbt[1], &arm.spsrAbt);', 'banks(0x17, &arm.registersUnd[0], &arm.registersUnd[1], &arm.spsrUnd);'), # 43: Abort mode gets Undefined's registers
    ('e.stw(9, offRegs + 14 * 4, R_ARM);\n', ''), # 44: A return doesn't swap R14
    ('e.call(addrRestoreCpsr);\n        e.call(codeRefresh);\n', 'e.call(addrRestoreCpsr);\n'), # 45: A return's slow path doesn't refresh
    ('                setDirty(sqrt ? addrSqrtDirty : addrDivDirty);\n            }\n            e.blr();\n            e.patchBc(next);\n        }\n    }\n    e.patchB(done);', '            }\n            e.blr();\n            e.patchBc(next);\n        }\n    }\n    e.patchB(done);'), # 46: Writing a math word leaves the old result
    ('if (!block->thumb && native && nativeKind(op, arm7) == NK_ALU_PC && (op & BIT(20))) {\n            differ', 'if (false) {\n            differ'), # 47: A return to THUMB at the next address stays in ARM
    ('e.rlwimi(7, 9, 0, 24, 24);\n', ''), # 48: Writing IE changes bit 7
    ('e.rlwinm_(9, 9, 7, 31, 31);', 'e.rlwinm_(9, 9, 6, 31, 31);'), # 49: Clearing IF checks the wrong FIFO condition
    ('e.lwz(8, offIrf + 4, R_ARM);', 'e.lwz(8, offIrf, R_ARM);'), # 50: Reading IF gets the high word
    ('    if ((op >> 28) != 0xE) reads = FLAG_ALL;\n', ''), # 51: A condition doesn't count as reading flags
    ('writes = always = logical ? (FLAG_N | FLAG_Z) : FLAG_ALL;', 'writes = always = FLAG_ALL;'), # 52: Logical operations count as setting C and V
    ('if ((opc >= 5 && opc <= 7) || rrx) reads |= FLAG_C;', 'if (rrx) reads |= FLAG_C;'), # 53: ADC, SBC and RSC don't count as reading the carry
    ('if ((opc >= 5 && opc <= 7) || rrx) reads |= FLAG_C;', 'if (opc >= 5 && opc <= 7) reads |= FLAG_C;'), # 54: RRX doesn't count as reading the carry
    ('if (logical && byRegister) writes |= FLAG_C;', 'if (logical && byRegister) writes = always = FLAG_N | FLAG_Z | FLAG_C;'), # 55: A shift by a register counts as always setting the carry
    ('else if (logical && (op & ((op & BIT(25)) ? 0xF00 : 0xFE0))) writes', 'else if (logical) writes'), # 56: A logical operation without a shift counts as setting the carry
    ('if (op & BIT(20)) writes = always = FLAG_N | FLAG_Z;\n    }\n    else if (kind == NK_MISC)', 'if (op & BIT(20)) writes = always = FLAG_ALL;\n    }\n    else if (kind == NK_MISC)'), # 57: A multiply counts as setting every flag
    ('if (!(op & BIT(4))) reads = FLAG_ALL; // MRS', ''), # 58: MRS doesn't count as reading flags
    ('if ((op & BIT(25)) && (op & 0xFF0) == 0x060) reads = FLAG_C;', ''), # 59: A load with an offset rotated through the carry doesn't count as reading it
    ('            live = FLAG_ALL;\n            continue;', '            continue;'), # 60: Flags don't count as read by what's outside a run
    ('flagsLive = liveFlags[i];', 'flagsLive = liveFlags[i ? i - 1 : 0];'), # 61: An instruction goes by the flags read after the one before it
    ('int need = s ? (writes & flagsLive) : 0;', 'int need = s ? (writes & flagsLive & ~FLAG_V) : 0;'), # 62: Arithmetic that's only needed for V sets no flags
    ('else if (need == FLAG_C) {', 'else if ((need & FLAG_C) && !(need & FLAG_N)) {'), # 63: Arithmetic needed for C and V only sets C
    ('e.rlwimi(6, 9, 29, 2, 2);', 'e.rlwimi(6, 9, 28, 3, 3);'), # 64: The carry on its own goes to V
    ('e.adde(9, 9, 9); // The host\'s carry in the low bit, whatever was there', 'e.add(9, 9, 9);'), # 65: The carry on its own is always clear
    ('bool setFlags = flags && (flagsLive & (FLAG_N | FLAG_Z));', 'bool setFlags = flags && !(flagsLive & (FLAG_N | FLAG_Z));'), # 66: A multiply sets flags only when nothing reads them
    ('e.li(3, flags ? 4 : 2);', 'e.li(3, setFlags ? 4 : 2);'), # 67: A multiply whose flags aren't read is quicker on the ARM9
    ('e.li(3, flags ? 5 : 3);', 'e.li(3, setFlags ? 5 : 3);'), # 68: A long multiply whose flags aren't read is quicker on the ARM9
    ("    if (host->entriesCycles[index]) e.li(R_BATCH, -int(host->entriesCycles[index])); // What the run's check adds for before here\n", ''), # 69: An entry into the middle of a run counts the cycles before it too
    ('            if (runCycles) e.addi(3, 3, runCycles);\n', ''), # 70: A run's cycles are lost when its check follows a condition
    ('e.addi(3, R_BATCH, 3 + runCycles);', 'e.addi(3, R_BATCH, 3);'), # 71: A conditional branch that's taken loses the cycles of the run before it
    ('e.addi(3, R_BATCH, 1 + runCycles);', 'e.addi(3, R_BATCH, 1);'), # 72: A conditional branch that isn't taken loses the cycles of the run before it
    ('            runCycles += *last & 0xFFFF;\n            e.ptr--;', '            e.ptr--;'), # 73: An instruction in a run isn't counted
    ('        e.patchBc(skip);\n        runCycles += 1;\n        return;', '        e.patchBc(skip);\n        return;'), # 74: A conditional instruction in a run isn't counted when its condition fails
    ('            e.addi(3, 3, -1);\n            e.add(R_BATCH, R_BATCH, 3);', '            e.add(R_BATCH, R_BATCH, 3);'), # 75: A conditional instruction in a run whose cycles vary counts one too many
    ('        e.rlwimi(6, 9, 25, 1, 1);\n        if (carry)', '        if (carry)'), # 76: Logical operations leave Z as it was
    ('        if (carry) e.rlwimi(6, 7, 29, 2, 2);\n', ''), # 77: Logical operations leave out the shifter's carry
    ('        e.rlwimi(6, 5, 0, 0, 0);\n        e.rlwimi(6, 8, 25, 1, 1);', '        e.rlwimi(6, 8, 25, 1, 1);'), # 78: Arithmetic leaves N as it was
    ('        e.rlwimi(6, 9, 29, 2, 2);\n        e.rlwimi(6, 10, 29, 3, 3);', '        e.rlwimi(6, 10, 29, 3, 3);'), # 79: Arithmetic leaves C as it was
    ('        e.rlwimi(6, 9, 29, 2, 2);\n        e.rlwimi(6, 10, 29, 3, 3);', '        e.rlwimi(6, 9, 29, 2, 2);'), # 80: Arithmetic leaves V as it was
    ('{ 0x4000, 0x2000, 0x8000, 0x1000 }; // Z, C, N, V', '{ 0x4000, 0x2000, 0x8000, 0x2000 }; // Z, C, N, V'), # 81: VS and VC go by the carry
    ('e.cmpwi(3, 1); // Equal if higher', 'e.cmpwi(3, 3); // Equal if higher'), # 82: HI needs Z set
    ('e.rlwinm(4, 3, 3, 0, 0); // V where N is', 'e.rlwinm(4, 3, 2, 0, 0); // V where N is'), # 83: GE, LT, GT and LE go by C in place of V
    ('                e.rlwinm(3, 3, 1, 0, 0); // Z where N is', '                e.li(3, 0);'), # 84: GT and LE leave out Z
    ('    else odd ? e.bge(nullptr) : e.blt(nullptr);', '    else odd ? e.blt(nullptr) : e.bge(nullptr);'), # 85: GE, LT, GT and LE are the wrong way round
    ('    else if (cond < 10) odd ? e.beq(nullptr) : e.bne(nullptr);', '    else if (cond < 10) odd ? e.bne(nullptr) : e.beq(nullptr);'), # 86: HI and LS are the wrong way round
    ('    e.li(R_NEXT, 0);\n', ''), # 87: Code that changed doesn't stop the block
    ('    e.rlwimi(4, 3, 27, 31, 31); // Cache key: the program counter with the THUMB bit\n', ''), # 88: The dispatcher's key leaves out THUMB
    ('    e.cmpw(7, 4);\n    e.mtctr(8);', '    e.cmpw(4, 4);\n    e.mtctr(8);'), # 89: The dispatcher takes whatever is in a key's place
    ('            e.andi_(9, 5, 3);\n            e.beqlr();', '            e.andi_(9, 5, 1);\n            e.beqlr();'), # 90: Words loaded from addresses misaligned by 2 aren't rotated
    ('e.cmplwi(7, 0x1000 - count * 4);', 'e.cmplwi(7, 0x1000 - 4);'), # 91: A list of registers crossing a page end is moved directly
    ('*target = stub + (listCalls[c].offset ? 1 : 0);', '*target = stub;'), # 92: A list of registers is always moved at its base
    ('    if (codePtr == codeStart) listStubs.clear(); // The area started over, so the routines are gone\n', ''), # 93: Routines for lists are kept when the code area starts over
    ('            e.rlwinm(6, R_TMP2, 22, 10, 29);\n            e.lwzx(6, 6, load ? R_RMAP : R_WMAP);', '            e.rlwinm(6, R_TMP2, 22, 10, 29);\n            e.lwzx(6, 6, R_RMAP);'), # 94: STM writes directly to pages that aren't mapped for writing
    ('                e.addi(5, R_TMP2, offset);\n                offset += 4;', '                e.addi(5, R_TMP2, offset);'), # 95: A list's slow path moves every register at the first address
    ('                    e.call(codeMemory[0]);\n                    if (user) e.stw(8, offUsr + i * 4, R_ARM);\n                    else writeReg(e, 8, i);', '                    e.call(codeMemory[0]);\n                    writeReg(e, 8, i);'), # 96: LDM with S loads the banked registers in the slow path
    ('                    if (user) e.lwz(8, offUsr + i * 4, R_ARM);\n                    else readReg(e, 8, i, 0);\n                    e.stwbrx(8, 0, 6);', '                    readReg(e, 8, i, 0);\n                    e.stwbrx(8, 0, 6);'), # 97: STM with S stores the banked registers
    ('e.addi(5, R_TMP2, (count - 1) * 4);', 'e.addi(5, R_TMP2, count * 4);'), # 98: LDM loads the PC from a word too far
    ('        e.addi(R_TMP2, R_TMP1, first);\n', '        e.mr(R_TMP2, R_TMP1);\n'), # 99: LDM of only the PC ignores its addressing mode
    ('(pc - 2 + ((int32_t)(block->instrs[i - 1].opcode << 21) >> 9)', '(pc + ((int32_t)(block->instrs[i - 1].opcode << 21) >> 9)'), # 100: A THUMB call after its first half goes 2 too far
    ('    if (thumbCallPair(host, index, arm7)) return nullptr;\n', ''), # 101: The second half of a THUMB call entered on its own goes where its first half would send it
    ('        if (!(op & BIT(12))) { // BLX to ARM\n            e.lwz(6, offCpsr, R_ARM);\n            e.rlwinm(6, 6, 0, 27, 25);', '        if (!(op & BIT(12))) { // BLX to ARM\n            e.lwz(6, offCpsr, R_ARM);\n            e.rlwinm(6, 6, 0, 0, 31);'), # 102: BLX after its first half stays in THUMB
    ('    if (callPair) {\n        // Calls to known addresses do too, and the ones to ARM change mode like BLX label\n        bool toThumb = raw & BIT(12);\n        block->links.push_back({ e.here(), toThumb ? ((callTarget & ~0x1) | 1) : (callTarget & ~0x3), nullptr, toThumb && sameLine(callTarget & ~0x1) });\n        e.b(codeDispatch);\n    }\n    else if (block->thumb', '    if (block->thumb'), # 103: A THUMB call after its first half doesn't link
    ('toThumb ? ((callTarget & ~0x1) | 1) : (callTarget & ~0x3), nullptr', '(callTarget & ~0x1) | 1, nullptr'), # 104: BLX after its first half links to THUMB code
    ('        e.loadConst(6, pc - 1);\n        writeReg(e, 6, 14);\n        if (!(op & BIT(12))) { // BLX to ARM', '        e.loadConst(6, pc + 1);\n        writeReg(e, 6, 14);\n        if (!(op & BIT(12))) { // BLX to ARM'), # 105: A THUMB call after its first half returns 2 too far
    ('e.call((op & BIT(22)) ? codeMultiplyCycles : codeMultiplyCyclesUnsigned);', 'e.call(codeMultiplyCycles);'), # 106: UMULL and UMLAL on the ARM7 take SMULL's timing, finishing early when the top bytes are all ones
]
# Bugs in the interpreter, which only the reference suite can see if the JIT has no native code for them
INTERPRETER_MUTATIONS = [
    ('arm/interpreter_alu.cpp', 'cpsr = (cpsr & ~0xF0000000) | (*op0 & BIT(31)) | ((*op0 == 0) << 30) | ((op2 >= *op0 &&', 'cpsr = (cpsr & ~0xC0000000) | (*op0 & BIT(31)) | ((*op0 == 0) << 30) | ((op2 >= *op0 &&'), # 107: RSCS can't clear C or V
    ('arm/interpreter_alu.cpp', ' | ((*op0 <= 0) << 29) |\n        ((op2 & *op0 & BIT(31)) >> 3);', ' | ((*op0 <= 0) << 29);'), # 108: NEG always clears V
    ('arm/interpreter_transfer.cpp', 'int Interpreter::ldmiaU(uint32_t opcode) { // LDMIA Rn, <Rlist>^\n    // User block load, post-increment without writeback; normal registers if branching\n    uint8_t m = bitCount[opcode & 0xFF] + bitCount[(opcode >> 8) & 0xFF];\n    uint32_t op0 = *registers[(opcode >> 16) & 0xF];\n    uint32_t **regs = &registers[(~opcode & BIT(15)) >> 11];', 'int Interpreter::ldmiaU(uint32_t opcode) { // LDMIA Rn, <Rlist>^\n    // User block load, post-increment without writeback; normal registers if branching\n    uint8_t m = bitCount[opcode & 0xFF] + bitCount[(opcode >> 8) & 0xFF];\n    uint32_t op0 = *registers[(opcode >> 16) & 0xF];\n    uint32_t **regs = &registers[0];'), # 109: LDM with S doesn't use user registers
    ('arm/interpreter_transfer.cpp', 'if ((opcode & BIT(16)) && (cpsr & 0x1F) != 0x10)\n        setCpsr', 'if (opcode & BIT(16))\n        setCpsr'), # 110: User mode MSR changes control bits
    ('arm/interpreter_transfer.cpp', '    if (spsr) setCpsr(*spsr);\n    flushPipeline();', '    if (spsr) setCpsr(*spsr);\n    cpsr |= (*registers[15] & !arm7) << 5;\n    flushPipeline();'), # 111: LDM with S takes THUMB from the PC
]
# Bugs only a game.sh option can see, numbered after those and left out of mutate.sh, which runs the cases above
GAME_MUTATIONS = [
    ('memory/memory.cpp', '        if (uint8_t *mapping = *(uint8_t *volatile*)&mappings[m])\n            value |= readLsb<T>(&mapping[address]);', '        value |= readLsb<T>(&mappings[m][address]);'), # game 112: a VRAM read follows a missing pointer, for vramstress
    ('io/spu.cpp', 'int32_t negate = (adpcmData >> 3) - 1;', 'int32_t negate = -int32_t(adpcmData >> 3);'), # game 113: ADPCM has its sign backwards, for audio; generated code and the interpreter share it, so compare with a run without it
    ('io/spu.cpp', 'float fraction = ringPosition - index;', 'float fraction = 0;'), # game 114: the ring's output doesn't interpolate, for ring; compare with a run without it too
]
if plant:
    text = open(path).read()
if mutation > len(MUTATIONS) + len(INTERPRETER_MUTATIONS):
    file, old, new = GAME_MUTATIONS[mutation - len(MUTATIONS) - len(INTERPRETER_MUTATIONS) - 1]
    change(file, (old, new))
elif mutation > len(MUTATIONS):
    file, old, new = INTERPRETER_MUTATIONS[mutation - len(MUTATIONS) - 1]
    change(file, (old, new))
elif mutation:
    old, new = MUTATIONS[mutation - 1]
    assert old in text, old
    text = text.replace(old, new)
if not plant:
    text += open(os.path.dirname(os.path.abspath(__file__)) + '/rig.inc').read()
open(path, 'w').write(text)
