#include "EmuState.h"
#include <stdlib.h>
#include "UartProtocol.h"
#include "Keyboard.h"
#include "fpgarom.h"
#include "FpgaCore.h"

EmuState emuState;

EmuState::EmuState() {
    memset(emuState.keybMatrix, 0xFF, sizeof(emuState.keybMatrix));

    for (unsigned i = 0; i < sizeof(emuState.mainRam); i++) {
        emuState.mainRam[i] = rand();
    }

    z80ctx.ioRead   = _ioRead;
    z80ctx.ioWrite  = _ioWrite;
    z80ctx.memRead  = _memRead;
    z80ctx.memWrite = _memWrite;

    loadFpgaCore(FpgaCoreType::AquariusPlus);
}

void EmuState::coldReset() {
    // Reset registers
    audioDAC              = 0;
    irqMask               = 0;
    irqStatus             = 0;
    bankRegs[0]           = 0xC0 | 0;
    bankRegs[1]           = 33;
    bankRegs[2]           = 34;
    bankRegs[3]           = 19;
    sysCtrlDisableExt     = false;
    sysCtrlAyDisable      = false;
    sysCtrlTurbo          = false;
    sysCtrlTurboUnlimited = false;
    soundOutput           = false;
    cpmRemap              = false;
    sysCtrlWarmBoot       = false;

    Z80RESET(&z80ctx);
    ay1.reset();
    ay2.reset();
    kbBufReset();

    emuMode = Em_Running;
}

void EmuState::warmReset() {
    coldReset();
    sysCtrlWarmBoot = true;
}

int EmuState::cpuEmulate() {
    bool haltAfterThis = false;

    if (enableDebugger) {
        if (tmpBreakpoint == z80ctx.PC) {
            tmpBreakpoint = -1;
            emuMode       = EmuState::Em_Halted;
            return 0;
        }

        if (enableBreakpoints) {
            for (int i = 0; i < (int)breakpoints.size(); i++) {
                auto &bp = breakpoints[i];
                if (bp.enabled && bp.type == 0 && bp.onX && z80ctx.PC == bp.addr && bp.addr != lastBpAddress) {
                    emuMode       = EmuState::Em_Halted;
                    lastBp        = i;
                    lastBpAddress = bp.addr;
                    return 0;
                }
            }
        }

        if (haltAfterRet >= 0) {
            uint8_t opcode = emuState.memRead(emuState.z80ctx.PC);
            if (opcode == 0xCD ||          // CALL nn
                (opcode & 0xC7) == 0xC4) { // CALL c,nn

                haltAfterRet++;

            } else if (
                opcode == 0xC9 ||          // RET
                (opcode & 0xC7) == 0xC7) { // RET cc

                haltAfterRet--;

                if (haltAfterRet < 0) {
                    haltAfterThis = true;
                    haltAfterRet  = -1;
                }
            }
        }
    }
    lastBp = -1;

    // Generate interrupt if needed
    if ((irqStatus & irqMask) != 0) {
        Z80INT(&z80ctx, 0xFF);
    }

    z80ctx.tstates = 0;
    Z80Execute(&z80ctx);
    int delta = z80ctx.tstates * 2;

    if (enableDebugger) {
        if (traceEnable && (!z80ctx.halted || !prevHalted)) {
            cpuTrace.emplace_back();
            auto &entry = cpuTrace.back();
            entry.pc    = z80ctx.PC;
            entry.r1    = z80ctx.R1;
            entry.r2    = z80ctx.R2;

            z80ctx.tstates = 0;
            Z80Debug(&z80ctx, entry.bytes, entry.instrStr);

            while ((int)cpuTrace.size() > traceDepth) {
                cpuTrace.pop_front();
            }
        }
        prevHalted = z80ctx.halted;

        if (haltAfterThis || (z80ctx.halted && stopOnHalt)) {
            emuMode = EmuState::Em_Halted;
        }
    }
    return delta;
}

unsigned EmuState::emulate() {
    unsigned resultFlags = 0;

    int delta = 0;
    {
        int deltaDiv = (emuState.sysCtrlTurbo) ? (emuState.sysCtrlTurboUnlimited ? 4 : 2) : 1;
        for (int i = 0; i < deltaDiv; i++) delta += cpuEmulate();
        delta /= deltaDiv;
    }

    int prevLineHalfCycles = lineHalfCycles;
    lineHalfCycles += delta;
    sampleHalfCycles += delta;

    // Handle VIRQLINE register
    if (prevLineHalfCycles < 320 && lineHalfCycles >= 320 && video.isOnVideoIrqLine()) {
        irqStatus |= (1 << 1);
    }

    if (lineHalfCycles >= HCYCLES_PER_LINE) {
        lineHalfCycles -= HCYCLES_PER_LINE;

        video.drawLine(video.getLine());

        if (video.nextLine()) {
            resultFlags |= ERF_RENDER_SCREEN;
        } else if (video.isOnStartOfVBlank()) {
            irqStatus |= (1 << 0);
        }
    }
    keyboardTypeIn();

    // Render audio?
    if (sampleHalfCycles >= HCYCLES_PER_SAMPLE) {
        sampleHalfCycles -= HCYCLES_PER_SAMPLE;

        // Take average of 5 AY8910 samples to match sampling rate (16*5*44100 = 3.528MHz)
        audioLeft  = 0;
        audioRight = 0;

        for (int i = 0; i < 5; i++) {
            uint16_t abc[3];
            ay1.render(abc);
            audioLeft += 2 * abc[0] + 2 * abc[1] + 1 * abc[2];
            audioRight += 1 * abc[0] + 2 * abc[1] + 2 * abc[2];

            ay2.render(abc);
            audioLeft += 2 * abc[0] + 2 * abc[1] + 1 * abc[2];
            audioRight += 1 * abc[0] + 2 * abc[1] + 2 * abc[2];

            audioLeft += (audioDAC << 4);
            audioRight += (audioDAC << 4);
        }

        uint16_t beep = soundOutput ? 10000 : 0;

        audioLeft  = audioLeft + beep;
        audioRight = audioRight + beep;
        resultFlags |= ERF_NEW_AUDIO_SAMPLE;
    }

    if (delta)
        lastBpAddress = -1;

    return resultFlags;
}

bool EmuState::loadCartridgeROM(const std::string &path) {
    auto ifs = std::ifstream(path, std::ifstream::binary);
    if (!ifs.good()) {
        return false;
    }

    ifs.seekg(0, ifs.end);
    auto fileSize = ifs.tellg();
    ifs.seekg(0, ifs.beg);

    if (fileSize == 8192) {
        ifs.read((char *)(cartRom + 8192), fileSize);
        // Mirror ROM to $C000
        memcpy(cartRom, cartRom + 8192, 8192);

    } else if (fileSize == 16384) {
        ifs.read((char *)cartRom, fileSize);

    } else {
        fprintf(stderr, "Invalid cartridge ROM file: %u, should be either exactly 8 or 16KB.\n", (unsigned)fileSize);
        return false;
    }
    ifs.close();

    cartridgeInserted = true;

    return true;
}

void EmuState::keyboardTypeIn() {
    if (emuState.kbBufCnt < 16 && !typeInStr.empty()) {
        char ch = typeInStr.front();
        typeInStr.erase(typeInStr.begin());
        Keyboard::instance()->pressKey(ch);
    }
}

uint8_t EmuState::memRead(uint16_t addr, bool triggerBp) {
    if (emuState.enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)emuState.breakpoints.size(); i++) {
            auto &bp = emuState.breakpoints[i];
            if (bp.enabled && bp.onR && bp.type == 0 && addr == bp.addr && bp.addr != emuState.lastBpAddress) {
                emuState.emuMode       = EmuState::Em_Halted;
                emuState.lastBp        = i;
                emuState.lastBpAddress = bp.addr;
            }
        }
    }

    // Handle CPM remap bit
    if (emuState.cpmRemap) {
        if (addr < 0x4000)
            addr += 0xC000;
        if (addr >= 0xC000)
            addr -= 0xC000;
    }

    // Get and decode banking register
    uint8_t  bankReg    = emuState.bankRegs[addr >> 14];
    unsigned page       = bankReg & 0x3F;
    bool     overlayRam = (bankReg & (1 << 6)) != 0;

    addr &= 0x3FFF;

    if (overlayRam && addr >= 0x3000 && addr < 0x3800) {
        return video.readScreenOrColorRam(addr);
    }

    if (page == 0) {
        if (addr < sizeof(fpgarom_start))
            return fpgarom_start[addr];
        return 0;
    } else if (page == 19) {
        return emuState.cartridgeInserted ? emuState.cartRom[addr] : 0xFF;
    } else if (page == 20) {
        return video.videoRam[addr];
    } else if (page == 21) {
        if (addr < 0x800) {
            return video.charRam[addr];
        }
    } else if (page >= 32 && page < 64) {
        return emuState.mainRam[(page - 32) * 0x4000 + addr];
    }
    return 0xFF;
}

void EmuState::memWrite(uint16_t addr, uint8_t data, bool triggerBp) {
    if (emuState.enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)emuState.breakpoints.size(); i++) {
            auto &bp = emuState.breakpoints[i];
            if (bp.enabled && bp.onW && bp.type == 0 && addr == bp.addr && bp.addr != emuState.lastBpAddress) {
                emuState.emuMode       = EmuState::Em_Halted;
                emuState.lastBp        = i;
                emuState.lastBpAddress = bp.addr;
            }
        }
    }

    // Handle CPM remap bit
    if (emuState.cpmRemap) {
        if (addr < 0x4000)
            addr += 0xC000;
        if (addr >= 0xC000)
            addr -= 0xC000;
    }

    // Get and decode banking register
    uint8_t  bankReg    = emuState.bankRegs[addr >> 14];
    unsigned page       = bankReg & 0x3F;
    bool     overlayRam = (bankReg & (1 << 6)) != 0;
    bool     readonly   = (bankReg & (1 << 7)) != 0;
    addr &= 0x3FFF;

    if (overlayRam && addr >= 0x3000 && addr < 0x3800) {
        video.writeScreenOrColorRam(addr, data);
        return;
    }

    if (readonly && !(overlayRam && addr >= 0x3800))
        return;

    if (page < 16) {
        // System ROM is readonly
        return;
    } else if (page == 19) {
        // Game ROM is readonly
        return;
    } else if (page == 20) {
        video.videoRam[addr] = data;
    } else if (page == 21) {
        if (addr < 0x800) {
            video.charRam[addr] = data;
        }
    } else if (page >= 32 && page < 64) {
        emuState.mainRam[(page - 32) * 0x4000 + addr] = data;
    }
}

uint8_t EmuState::ioRead(uint16_t addr, bool triggerBp) {
    uint8_t addr8 = addr & 0xFF;

    if (emuState.enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)emuState.breakpoints.size(); i++) {
            auto &bp = emuState.breakpoints[i];
            if (bp.enabled && bp.onR && ((bp.type == 1 && (addr & 0xFF) == (bp.addr & 0xFF)) || (bp.type == 2 && addr == bp.addr))) {
                emuState.emuMode = EmuState::Em_Halted;
                emuState.lastBp  = i;
            }
        }
    }

    if (!emuState.sysCtrlDisableExt) {
        if (addr8 >= 0xE0 && addr8 <= 0xED) {
            return video.readReg(addr8);
        }
        switch (addr8) {
            case 0xEE: return emuState.irqMask;
            case 0xEF: return emuState.irqStatus;
            case 0xF0: return emuState.bankRegs[0];
            case 0xF1: return emuState.bankRegs[1];
            case 0xF2: return emuState.bankRegs[2];
            case 0xF3: return emuState.bankRegs[3];
            case 0xF4: return UartProtocol::instance()->readCtrl();
            case 0xF5: return UartProtocol::instance()->readData();
        }
    }

    switch (addr8) {
        case 0xF6:
        case 0xF7:
            if (!emuState.sysCtrlAyDisable)
                return emuState.ay1.read();
            return 0xFF;

        case 0xF8:
        case 0xF9:
            if (!(emuState.sysCtrlAyDisable || emuState.sysCtrlDisableExt))
                return emuState.ay2.read();
            return 0xFF;

        case 0xFA: return emuState.kbBufRead();
        case 0xFB: return (
            (emuState.sysCtrlWarmBoot ? (1 << 7) : 0) |
            (emuState.sysCtrlTurboUnlimited ? (1 << 3) : 0) |
            (emuState.sysCtrlTurbo ? (1 << 2) : 0) |
            (emuState.sysCtrlAyDisable ? (1 << 1) : 0) |
            (emuState.sysCtrlDisableExt ? (1 << 0) : 0));

        case 0xFC: /* printf("Cassette port input (%04x)\n", addr); */ return 0xFF;
        case 0xFD: return video.readReg(addr & 0xFF);
        case 0xFE: /* printf("Clear to send status (%04x)\n", addr); */ return 0xFF;
        case 0xFF: {
            // Keyboard matrix. Selected rows are passed in the upper 8 address lines.
            uint8_t rows = addr >> 8;

            // Wire-AND all selected rows.
            uint8_t result = 0xFF;
            for (int i = 0; i < 8; i++) {
                if ((rows & (1 << i)) == 0) {
                    result &= emuState.keybMatrix[i];
                }
            }
            return result;
        }
        default: break;
    }

    printf("ioRead(0x%02x)\n", addr8);
    return 0xFF;
}

void EmuState::ioWrite(uint16_t addr, uint8_t data, bool triggerBp) {
    uint8_t addr8 = addr & 0xFF;

    if (emuState.enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)emuState.breakpoints.size(); i++) {
            auto &bp = emuState.breakpoints[i];
            if (bp.enabled && bp.onW && ((bp.type == 1 && addr8 == (bp.addr & 0xFF)) || (bp.type == 2 && addr == bp.addr))) {
                emuState.emuMode = EmuState::Em_Halted;
                emuState.lastBp  = i;
            }
        }
    }

    if (!emuState.sysCtrlDisableExt) {
        if ((addr8 >= 0xE0 && addr8 <= 0xEB) || (addr8 == 0xED)) {
            video.writeReg(addr8, data);
            return;
        }

        switch (addr8) {
            case 0xEC: emuState.audioDAC = data; return;
            case 0xEE: emuState.irqMask = data & 3; return;
            case 0xEF: emuState.irqStatus &= ~data; return;
            case 0xF0: emuState.bankRegs[0] = data; return;
            case 0xF1: emuState.bankRegs[1] = data; return;
            case 0xF2: emuState.bankRegs[2] = data; return;
            case 0xF3: emuState.bankRegs[3] = data; return;
            case 0xF4: UartProtocol::instance()->writeCtrl(data); return;
            case 0xF5: UartProtocol::instance()->writeData(data); return;
            case 0xFA: emuState.kbBufReset(); return;
        }
    }

    switch (addr8) {
        case 0xF6:
        case 0xF7:
            if (!emuState.sysCtrlAyDisable)
                emuState.ay1.write(addr8, data);
            return;

        case 0xF8:
        case 0xF9:
            if (!(emuState.sysCtrlAyDisable || emuState.sysCtrlDisableExt))
                emuState.ay2.write(addr8, data);
            return;

        case 0xFB:
            emuState.sysCtrlDisableExt     = (data & (1 << 0)) != 0;
            emuState.sysCtrlAyDisable      = (data & (1 << 1)) != 0;
            emuState.sysCtrlTurbo          = (data & (1 << 2)) != 0;
            emuState.sysCtrlTurboUnlimited = (data & (1 << 3)) != 0;
            if ((data & (1 << 7)) != 0)
                emuState.warmReset();
            return;

        case 0xFC: emuState.soundOutput = (data & 1) != 0; break;
        case 0xFD: emuState.cpmRemap = (data & 1) != 0; break;
        case 0xFE: /* printf("1200 bps serial printer (%04x) = %u\n", addr, data & 1); */ break;
        case 0xFF: break;
        default: printf("ioWrite(0x%02x, 0x%02x)\n", addr8, data); break;
    }
}

void EmuState::kbBufReset() {
    kbBufCnt   = 0;
    kbBufRdIdx = 0;
    kbBufWrIdx = 0;
}

void EmuState::kbBufWrite(uint8_t val) {
    if (kbBufCnt < sizeof(kbBuf)) {
        kbBufCnt++;
        kbBuf[kbBufWrIdx++] = val;
        if (kbBufWrIdx == sizeof(kbBuf))
            kbBufWrIdx = 0;
    }
}

uint8_t EmuState::kbBufRead() {
    uint8_t result = 0;
    if (kbBufCnt > 0) {
        result = kbBuf[kbBufRdIdx++];
        if (kbBufRdIdx == sizeof(kbBuf))
            kbBufRdIdx = 0;
        kbBufCnt--;
    }
    return result;
}
