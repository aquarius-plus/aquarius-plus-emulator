#include "AqpEmuState.h"
#include "FPGA.h"
#include "UartProtocol.h"
#include "fpgarom.h"
#include "FpgaCore.h"
#include "Keyboard.h"
#include "imgui.h"
#include "tinyfiledialogs.h"

// 3579545 Hz -> 59659 cycles / frame
// 7159090 Hz -> 119318 cycles / frame

// 455x262=119210 -> 60.05 Hz
// 51.2us + 1.5us + 4.7us + 6.2us = 63.6 us
// 366 active pixels

#define HCYCLES_PER_LINE   (455)
#define HCYCLES_PER_SAMPLE (162)

AqpEmuState::AqpEmuState() {
    memset(keybMatrix, 0xFF, sizeof(keybMatrix));

    for (unsigned i = 0; i < sizeof(mainRam); i++)
        mainRam[i] = rand();

    z80ctx.ioRead   = _ioRead;
    z80ctx.ioWrite  = _ioWrite;
    z80ctx.ioParam  = reinterpret_cast<uintptr_t>(this);
    z80ctx.memRead  = _memRead;
    z80ctx.memWrite = _memWrite;
    z80ctx.memParam = reinterpret_cast<uintptr_t>(this);

    loadFpgaCore(FpgaCoreType::AquariusPlus);

    reset(true);
}

void AqpEmuState::loadConfig(cJSON *root) {
    auto cfgBreakpoints = cJSON_GetObjectItem(root, "breakpoints");
    if (cJSON_IsArray(cfgBreakpoints)) {
        cJSON *breakpoint;
        cJSON_ArrayForEach(breakpoint, cfgBreakpoints) {
            if (!cJSON_IsObject(breakpoint))
                continue;

            Breakpoint bp;
            bp.addr    = getIntValue(breakpoint, "addr", 0);
            bp.name    = getStringValue(breakpoint, "name", "");
            bp.enabled = getBoolValue(breakpoint, "enabled", false);
            bp.type    = getIntValue(breakpoint, "type", 0);
            bp.onR     = getBoolValue(breakpoint, "onR", false);
            bp.onW     = getBoolValue(breakpoint, "onW", false);
            bp.onX     = getBoolValue(breakpoint, "onX", false);
            breakpoints.push_back(bp);
        }
    }
    enableBreakpoints = getBoolValue(root, "enableBreakpoints", false);
    traceEnable       = getBoolValue(root, "traceEnable", false);
    traceDepth        = getIntValue(root, "traceDepth", 16);

    auto cfgWatches = cJSON_GetObjectItem(root, "watches");
    if (cJSON_IsArray(cfgWatches)) {
        cJSON *watch;
        cJSON_ArrayForEach(watch, cfgWatches) {
            if (!cJSON_IsObject(watch))
                continue;

            Watch w;
            w.addr = getIntValue(watch, "addr", 0);
            w.name = getStringValue(watch, "name", "");
            w.type = (WatchType)getIntValue(watch, "type", 0);
            watches.push_back(w);
        }
    }
}

void AqpEmuState::saveConfig(cJSON *root) {
    cJSON_AddBoolToObject(root, "enableBreakpoints", enableBreakpoints);
    cJSON_AddBoolToObject(root, "traceEnable", traceEnable);
    cJSON_AddNumberToObject(root, "traceDepth", traceDepth);

    auto cfgBreakpoints = cJSON_AddArrayToObject(root, "breakpoints");
    for (auto &bp : breakpoints) {
        auto breakpoint = cJSON_CreateObject();

        cJSON_AddNumberToObject(breakpoint, "addr", bp.addr);
        cJSON_AddStringToObject(breakpoint, "name", bp.name.c_str());
        cJSON_AddBoolToObject(breakpoint, "enabled", bp.enabled);
        cJSON_AddNumberToObject(breakpoint, "type", bp.type);
        cJSON_AddBoolToObject(breakpoint, "onR", bp.onR);
        cJSON_AddBoolToObject(breakpoint, "onW", bp.onW);
        cJSON_AddBoolToObject(breakpoint, "onX", bp.onX);
        cJSON_AddItemToArray(cfgBreakpoints, breakpoint);
    }

    auto cfgWatches = cJSON_AddArrayToObject(root, "watches");
    for (auto &w : watches) {
        auto watch = cJSON_CreateObject();
        cJSON_AddNumberToObject(watch, "addr", w.addr);
        cJSON_AddStringToObject(watch, "name", w.name.c_str());
        cJSON_AddNumberToObject(watch, "type", (int)w.type);
        cJSON_AddItemToArray(cfgWatches, watch);
    }
}

void AqpEmuState::reset(bool cold) {
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
    sysCtrlWarmBoot       = !cold;

    Z80RESET(&z80ctx);
    ay1.reset();
    ay2.reset();
    kbBufReset();

    emuMode = Em_Running;
}

bool AqpEmuState::loadCartridgeROM(const std::string &path) {
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

uint8_t AqpEmuState::memRead(uint16_t addr, bool triggerBp) {
    if (enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)breakpoints.size(); i++) {
            auto &bp = breakpoints[i];
            if (bp.enabled && bp.onR && bp.type == 0 && addr == bp.addr && bp.addr != lastBpAddress) {
                emuMode       = Em_Halted;
                lastBp        = i;
                lastBpAddress = bp.addr;
            }
        }
    }

    // Handle CPM remap bit
    if (cpmRemap) {
        if (addr < 0x4000)
            addr += 0xC000;
        if (addr >= 0xC000)
            addr -= 0xC000;
    }

    // Get and decode banking register
    uint8_t  bankReg    = bankRegs[addr >> 14];
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
        return cartridgeInserted ? cartRom[addr] : 0xFF;
    } else if (page == 20) {
        return video.videoRam[addr];
    } else if (page == 21) {
        if (addr < 0x800) {
            return video.charRam[addr];
        }
    } else if (page >= 32 && page < 64) {
        return mainRam[(page - 32) * 0x4000 + addr];
    }
    return 0xFF;
}

void AqpEmuState::memWrite(uint16_t addr, uint8_t data, bool triggerBp) {
    if (enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)breakpoints.size(); i++) {
            auto &bp = breakpoints[i];
            if (bp.enabled && bp.onW && bp.type == 0 && addr == bp.addr && bp.addr != lastBpAddress) {
                emuMode       = Em_Halted;
                lastBp        = i;
                lastBpAddress = bp.addr;
            }
        }
    }

    // Handle CPM remap bit
    if (cpmRemap) {
        if (addr < 0x4000)
            addr += 0xC000;
        if (addr >= 0xC000)
            addr -= 0xC000;
    }

    // Get and decode banking register
    uint8_t  bankReg    = bankRegs[addr >> 14];
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
        mainRam[(page - 32) * 0x4000 + addr] = data;
    }
}

uint8_t AqpEmuState::ioRead(uint16_t addr, bool triggerBp) {
    uint8_t addr8 = addr & 0xFF;

    if (enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)breakpoints.size(); i++) {
            auto &bp = breakpoints[i];
            if (bp.enabled && bp.onR && ((bp.type == 1 && (addr & 0xFF) == (bp.addr & 0xFF)) || (bp.type == 2 && addr == bp.addr))) {
                emuMode = Em_Halted;
                lastBp  = i;
            }
        }
    }

    if (!sysCtrlDisableExt) {
        if (addr8 >= 0xE0 && addr8 <= 0xED) {
            return video.readReg(addr8);
        }
        switch (addr8) {
            case 0xEE: return irqMask;
            case 0xEF: return irqStatus;
            case 0xF0: return bankRegs[0];
            case 0xF1: return bankRegs[1];
            case 0xF2: return bankRegs[2];
            case 0xF3: return bankRegs[3];
            case 0xF4: return UartProtocol::instance()->readCtrl();
            case 0xF5: return UartProtocol::instance()->readData();
        }
    }

    switch (addr8) {
        case 0xF6:
        case 0xF7:
            if (!sysCtrlAyDisable)
                return ay1.read();
            return 0xFF;

        case 0xF8:
        case 0xF9:
            if (!(sysCtrlAyDisable || sysCtrlDisableExt))
                return ay2.read();
            return 0xFF;

        case 0xFA: return kbBufRead();
        case 0xFB: return (
            (sysCtrlWarmBoot ? (1 << 7) : 0) |
            (sysCtrlTurboUnlimited ? (1 << 3) : 0) |
            (sysCtrlTurbo ? (1 << 2) : 0) |
            (sysCtrlAyDisable ? (1 << 1) : 0) |
            (sysCtrlDisableExt ? (1 << 0) : 0));

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
                    result &= keybMatrix[i];
                }
            }
            return result;
        }
        default: break;
    }

    printf("ioRead(0x%02x)\n", addr8);
    return 0xFF;
}

void AqpEmuState::ioWrite(uint16_t addr, uint8_t data, bool triggerBp) {
    uint8_t addr8 = addr & 0xFF;

    if (enableBreakpoints && triggerBp) {
        for (int i = 0; i < (int)breakpoints.size(); i++) {
            auto &bp = breakpoints[i];
            if (bp.enabled && bp.onW && ((bp.type == 1 && addr8 == (bp.addr & 0xFF)) || (bp.type == 2 && addr == bp.addr))) {
                emuMode = Em_Halted;
                lastBp  = i;
            }
        }
    }

    if (!sysCtrlDisableExt) {
        if ((addr8 >= 0xE0 && addr8 <= 0xEB) || (addr8 == 0xED)) {
            video.writeReg(addr8, data);
            return;
        }

        switch (addr8) {
            case 0xEC: audioDAC = data; return;
            case 0xEE: irqMask = data & 3; return;
            case 0xEF: irqStatus &= ~data; return;
            case 0xF0: bankRegs[0] = data; return;
            case 0xF1: bankRegs[1] = data; return;
            case 0xF2: bankRegs[2] = data; return;
            case 0xF3: bankRegs[3] = data; return;
            case 0xF4: UartProtocol::instance()->writeCtrl(data); return;
            case 0xF5: UartProtocol::instance()->writeData(data); return;
            case 0xFA: kbBufReset(); return;
        }
    }

    switch (addr8) {
        case 0xF6:
        case 0xF7:
            if (!sysCtrlAyDisable)
                ay1.write(addr8, data);
            return;

        case 0xF8:
        case 0xF9:
            if (!(sysCtrlAyDisable || sysCtrlDisableExt))
                ay2.write(addr8, data);
            return;

        case 0xFB:
            sysCtrlDisableExt     = (data & (1 << 0)) != 0;
            sysCtrlAyDisable      = (data & (1 << 1)) != 0;
            sysCtrlTurbo          = (data & (1 << 2)) != 0;
            sysCtrlTurboUnlimited = (data & (1 << 3)) != 0;
            if ((data & (1 << 7)) != 0)
                reset(false);
            return;

        case 0xFC: soundOutput = (data & 1) != 0; break;
        case 0xFD: cpmRemap = (data & 1) != 0; break;
        case 0xFE: /* printf("1200 bps serial printer (%04x) = %u\n", addr, data & 1); */ break;
        case 0xFF: break;
        default: printf("ioWrite(0x%02x, 0x%02x)\n", addr8, data); break;
    }
}

int AqpEmuState::cpuEmulate() {
    bool haltAfterThis = false;

    if (enableDebugger) {
        if (tmpBreakpoint == z80ctx.PC) {
            tmpBreakpoint = -1;
            emuMode       = Em_Halted;
            return 0;
        }

        if (enableBreakpoints) {
            for (int i = 0; i < (int)breakpoints.size(); i++) {
                auto &bp = breakpoints[i];
                if (bp.enabled && bp.type == 0 && bp.onX && z80ctx.PC == bp.addr && bp.addr != lastBpAddress) {
                    emuMode       = Em_Halted;
                    lastBp        = i;
                    lastBpAddress = bp.addr;
                    return 0;
                }
            }
        }

        if (haltAfterRet >= 0) {
            uint8_t opcode = memRead(z80ctx.PC);
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

        auto config = Config::instance();
        if (haltAfterThis || (z80ctx.halted && config->stopOnHalt)) {
            emuMode = Em_Halted;
        }
    }
    return delta;
}

void AqpEmuState::getPixels(void *pixels, int pitch) {
    const uint16_t *fb = video.getFb();

    for (int j = 0; j < VIDEO_HEIGHT * 2; j++) {
        for (int i = 0; i < VIDEO_WIDTH; i++) {
            // Convert from RGB444 to ABGR888
            uint16_t col444 = fb[j / 2 * VIDEO_WIDTH + i];

            unsigned r4 = (col444 >> 8) & 0xF;
            unsigned g4 = (col444 >> 4) & 0xF;
            unsigned b4 = (col444 >> 0) & 0xF;

            unsigned r8 = (r4 << 4) | r4;
            unsigned g8 = (g4 << 4) | g4;
            unsigned b8 = (b4 << 4) | b4;

            ((uint32_t *)((uintptr_t)pixels + j * pitch))[i] = (0xFF << 24) | (b8 << 16) | (g8 << 8) | (r8);
        }
    }
}

unsigned AqpEmuState::emulate2() {
    unsigned resultFlags = 0;

    int delta = 0;
    {
        int deltaDiv = sysCtrlTurbo ? (sysCtrlTurboUnlimited ? 4 : 2) : 1;
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

bool AqpEmuState::emulate(int16_t *audioBuf, unsigned numSamples) {
    if (!enableDebugger) {
        // Always run when not debugging
        emuMode = Em_Running;
    }

    bool result = false;

    bool dbgUpdateScreen = false;
    if (emuMode == Em_Running) {
        dbgUpdateScreen = true;

        // Render each audio sample
        for (unsigned aidx = 0; aidx < numSamples; aidx++) {
            while (true) {
                auto flags = emulate2();
                if (flags & ERF_RENDER_SCREEN)
                    result = true;
                if (emuMode != Em_Running)
                    break;

                if (flags & ERF_NEW_AUDIO_SAMPLE)
                    break;
            }
            if (emuMode != Em_Running)
                break;

            if (audioBuf != nullptr) {
                float l = audioLeft / 65535.0f;
                float r = audioRight / 65535.0f;
                l       = dcBlockLeft.filter(l);
                r       = dcBlockRight.filter(r);
                l       = std::min(std::max(l, -1.0f), 1.0f);
                r       = std::min(std::max(r, -1.0f), 1.0f);

                audioBuf[aidx * 2 + 0] = (int16_t)(l * 32767.0f);
                audioBuf[aidx * 2 + 1] = (int16_t)(r * 32767.0f);
            }
        }
    }

    if (emuMode != Em_Running) {
        haltAfterRet  = -1;
        tmpBreakpoint = -1;
        if (emuMode == Em_Step) {
            dbgUpdateScreen = true;
            emuMode         = Em_Halted;
            emulate2();
        }

        if (dbgUpdateScreen) {
            dbgUpdateScreen = false;

            // Update screen
            for (int i = 0; i < 240; i++)
                video.drawLine(i);

            result = true;
        }
    }
    return result;
}

void AqpEmuState::keyboardTypeIn() {
    if (kbBufCnt < 16 && !typeInStr.empty()) {
        char ch = typeInStr.front();
        typeInStr.erase(typeInStr.begin());
        Keyboard::instance()->pressKey(ch);
    }
}

void AqpEmuState::kbBufReset() {
    kbBufCnt   = 0;
    kbBufRdIdx = 0;
    kbBufWrIdx = 0;
}

void AqpEmuState::kbBufWrite(uint8_t val) {
    if (kbBufCnt < sizeof(kbBuf)) {
        kbBufCnt++;
        kbBuf[kbBufWrIdx++] = val;
        if (kbBufWrIdx == sizeof(kbBuf))
            kbBufWrIdx = 0;
    }
}

uint8_t AqpEmuState::kbBufRead() {
    uint8_t result = 0;
    if (kbBufCnt > 0) {
        result = kbBuf[kbBufRdIdx++];
        if (kbBufRdIdx == sizeof(kbBuf))
            kbBufRdIdx = 0;
        kbBufCnt--;
    }
    return result;
}

void AqpEmuState::spiSel(bool enable) {
    if (enabled == enable)
        return;
    enabled = enable;

    if (!enabled && !txBuf.empty()) {
        switch (txBuf[0]) {
            case CMD_RESET: {
                if (txBuf.size() >= 1 + 1) {
                    if (txBuf[1] & 2) {
                        reset(true);
                    } else {
                        reset(false);
                    }
                }
                break;
            }

            case CMD_SET_KEYB_MATRIX: {
                if (txBuf.size() >= 1 + 8) {
                    memcpy(&keybMatrix, &txBuf[1], 8);
                }
                break;
            }

            case CMD_SET_HCTRL: {
                if (txBuf.size() >= 1 + 2) {
                    ay1.portRdData[0] = txBuf[1];
                    ay1.portRdData[1] = txBuf[2];
                }
                break;
            }

            case CMD_WRITE_KBBUF: {
                if (txBuf.size() >= 1 + 1) {
                    kbBufWrite(txBuf[1]);
                }
                break;
            }
        }
    }

    txBuf.clear();
}

void AqpEmuState::spiTx(const void *data, size_t length) {
    if (!enabled)
        return;
    auto p = static_cast<const uint8_t *>(data);
    txBuf.insert(txBuf.end(), p, p + length);
}

void AqpEmuState::spiRx(void *buf, size_t length) {
    if (!enabled)
        return;
}

void AqpEmuState::fileMenu() {
    if (ImGui::MenuItem("Load cartridge ROM...", "")) {
        char const *lFilterPatterns[1] = {"*.rom"};
        char       *romFile            = tinyfd_openFileDialog("Open ROM file", "", 1, lFilterPatterns, "ROM files", 0);
        if (romFile) {
            if (loadCartridgeROM(romFile)) {
                reset(true);
            }
        }
    }
    if (ImGui::MenuItem("Eject cartridge", "", false, cartridgeInserted)) {
        cartridgeInserted = false;
        reset(true);
    }
    ImGui::Separator();
}

void AqpEmuState::dbgMenu() {
    if (!enableDebugger)
        return;

    auto config = Config::instance();

    ImGui::MenuItem("Memory editor", "", &config->showMemEdit);
    ImGui::MenuItem("CPU state", "", &config->showCpuState);
    ImGui::MenuItem("IO Registers", "", &config->showIoRegsWindow);
    ImGui::MenuItem("Breakpoints", "", &config->showBreakpoints);
    ImGui::MenuItem("Assembly listing", "", &config->showAssemblyListing);
    ImGui::MenuItem("CPU trace", "", &config->showCpuTrace);
    ImGui::MenuItem("Watch", "", &config->showWatch);
    ImGui::MenuItem("Stop on HALT instruction", "", &config->stopOnHalt);
}

void AqpEmuState::dbgWindows() {
    if (!enableDebugger)
        return;

    auto config = Config::instance();

    if (emuMode == Em_Halted)
        config->showCpuState = true;

    if (config->showMemEdit)
        dbgWndMemEdit(&config->showMemEdit);
    if (config->showCpuState)
        dbgWndCpuState(&config->showCpuState);
    if (config->showIoRegsWindow)
        dbgWndIoRegs(&config->showIoRegsWindow);
    if (config->showBreakpoints)
        dbgWndBreakpoints(&config->showBreakpoints);
    if (config->showAssemblyListing)
        dbgWndAssemblyListing(&config->showAssemblyListing);
    if (config->showCpuTrace)
        dbgWndCpuTrace(&config->showCpuTrace);
    if (config->showWatch)
        dbgWndWatch(&config->showWatch);
}

void AqpEmuState::dbgWndCpuState(bool *p_open) {
    if (ImGui::Begin("CPU state", p_open, ImGuiWindowFlags_AlwaysAutoResize)) {

        ImGui::PushStyleColor(ImGuiCol_Button, emuMode == Em_Halted ? (ImVec4)ImColor(192, 0, 0) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
        ImGui::BeginDisabled(emuMode != Em_Running);
        ImGui::SetNextItemShortcut(ImGuiMod_Shift | ImGuiKey_F5, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Tooltip);
        if (ImGui::Button("Halt")) {
            emuMode = Em_Halted;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor();

        ImGui::BeginDisabled(emuMode == Em_Running);
        ImGui::SameLine();

        ImGui::SetNextItemShortcut(ImGuiKey_F11, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Tooltip);
        if (ImGui::Button("Step Into")) {
            emuMode = Em_Step;
        }
        ImGui::SameLine();

        ImGui::SetNextItemShortcut(ImGuiKey_F10, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Tooltip);
        if (ImGui::Button("Step Over")) {
            int tmpBreakpoint = -1;

            if (z80ctx.halted) {
                // Step over HALT instruction
                z80ctx.halted = 0;
                z80ctx.PC++;
            } else {
                uint8_t opcode = memRead(z80ctx.PC);
                if (opcode == 0xCD ||          // CALL nn
                    (opcode & 0xC7) == 0xC4) { // CALL c,nn

                    tmpBreakpoint = z80ctx.PC + 3;

                } else if ((opcode & 0xC7) == 0xC7) { // RST
                    tmpBreakpoint = z80ctx.PC + 1;
                    if ((opcode & 0x38) == 0x08 ||
                        (opcode & 0x38) == 0x30) {

                        // Skip one extra byte on RST 08H/30H, since on the Aq these
                        // system calls absorb the byte following this instruction.
                        tmpBreakpoint++;
                    }

                } else if (opcode == 0xED) {
                    opcode = memRead(z80ctx.PC + 1);
                    if (opcode == 0xB9 || // CPDR
                        opcode == 0xB1 || // CPIR
                        opcode == 0xBA || // INDR
                        opcode == 0xB2 || // INIR
                        opcode == 0xB8 || // LDDR
                        opcode == 0xB0 || // LDIR
                        opcode == 0xBB || // OTDR
                        opcode == 0xB3) { // OTIR
                    }
                    tmpBreakpoint = z80ctx.PC + 2;
                }
                tmpBreakpoint = tmpBreakpoint;
                if (tmpBreakpoint >= 0) {
                    emuMode = Em_Running;
                } else {
                    emuMode = Em_Step;
                }
            }
        }

        ImGui::SameLine();
        ImGui::SetNextItemShortcut(ImGuiMod_Shift | ImGuiKey_F10, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Tooltip);
        if (ImGui::Button("Step Out")) {
            haltAfterRet = 0;
            emuMode      = Em_Running;
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, emuMode == Em_Running ? (ImVec4)ImColor(0, 128, 0) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
        ImGui::SetNextItemShortcut(ImGuiKey_F5, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Tooltip);
        if (ImGui::Button("Go")) {
            emuMode = Em_Running;
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();

        ImGui::Separator();

        {
            uint16_t    addr = z80ctx.PC;
            std::string name;
            if (asmListing.findNearestSymbol(addr, name)) {
                ImGui::Text("%s ($%04X + %u)", name.c_str(), addr, z80ctx.PC - addr);
                ImGui::Separator();
            }
        }

        {
            char tmp1[64];
            char tmp2[64];
            z80ctx.tstates = 0;

            bool prevEnableBp = enableBreakpoints;
            enableBreakpoints = false;
            Z80Debug(&z80ctx, tmp1, tmp2);
            enableBreakpoints = prevEnableBp;

            ImGui::Text("         %-12s %s", tmp1, tmp2);
        }
        ImGui::Separator();

        auto drawAddrVal = [&](const std::string &name, uint16_t val) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%-3s", name.c_str());
            ImGui::TableNextColumn();

            if (emuMode == Em_Running) {
                ImGui::Text("%04X", val);
            } else {

                char addr[32];
                snprintf(addr, sizeof(addr), "%04X##%s", val, name.c_str());
                ImGui::Selectable(addr);
                addrPopup(val);
            }

            ImGui::TableNextColumn();

            uint8_t data[8];
            for (int i = 0; i < 8; i++)
                data[i] = memRead(val + i);
            ImGui::Text(
                "%02X %02X %02X %02X %02X %02X %02X %02X",
                memRead(val + 0),
                memRead(val + 1),
                memRead(val + 2),
                memRead(val + 3),
                memRead(val + 4),
                memRead(val + 5),
                memRead(val + 6),
                memRead(val + 7));

            ImGui::TableNextColumn();
            std::string str;
            for (int i = 0; i < 8; i++)
                str.push_back((data[i] >= 32 && data[i] <= 0x7E) ? data[i] : '.');
            ImGui::TextUnformatted(str.c_str());
        };

        auto drawAF = [](const std::string &name, uint16_t val) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%-3s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%04X", val);
            ImGui::TableNextColumn();

            auto a = val >> 8;
            auto f = val & 0xFF;

            ImGui::Text(
                "    %c %c %c %c %c %c %c %c", //      %c",
                (f & 0x80) ? 'S' : '-',
                (f & 0x40) ? 'Z' : '-',
                (f & 0x20) ? 'X' : '-',
                (f & 0x10) ? 'H' : '-',
                (f & 0x08) ? 'X' : '-',
                (f & 0x04) ? 'P' : '-',
                (f & 0x02) ? 'N' : '-',
                (f & 0x01) ? 'C' : '-');

            ImGui::TableNextColumn();
            ImGui::Text("%c", (a >= 32 && a <= 0x7E) ? a : '.');
        };

        if (ImGui::BeginTable("RegTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthFixed);
            // ImGui::TableHeadersRow();

            drawAddrVal("PC", z80ctx.PC);
            drawAddrVal("SP", z80ctx.R1.wr.SP);
            drawAF("AF", z80ctx.R1.wr.AF);
            drawAddrVal("BC", z80ctx.R1.wr.BC);
            drawAddrVal("DE", z80ctx.R1.wr.DE);
            drawAddrVal("HL", z80ctx.R1.wr.HL);
            drawAddrVal("IX", z80ctx.R1.wr.IX);
            drawAddrVal("IY", z80ctx.R1.wr.IY);
            drawAF("AF'", z80ctx.R2.wr.AF);
            drawAddrVal("BC'", z80ctx.R2.wr.BC);
            drawAddrVal("DE'", z80ctx.R2.wr.DE);
            drawAddrVal("HL'", z80ctx.R2.wr.HL);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("IR");
            ImGui::TableNextColumn();
            ImGui::Text("%04X", (z80ctx.I << 8) | z80ctx.R);
            ImGui::TableNextColumn();
            ImGui::Text("IM %u  Interrupts %3s", z80ctx.IM, z80ctx.IFF1 ? "On" : "Off");
            ImGui::TableNextColumn();

            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void AqpEmuState::dbgWndIoRegs(bool *p_open) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(330, 132), ImVec2(330, FLT_MAX));
    if (ImGui::Begin("IO Registers", p_open, 0)) {
        if (ImGui::CollapsingHeader("Video")) {
            video.dbgDrawIoRegs();
        }
        if (ImGui::CollapsingHeader("Interrupt")) {
            ImGui::Text("$EE IRQMASK: $%02X %s%s", irqMask, irqMask & 2 ? "[LINE]" : "", irqMask & 1 ? "[VBLANK]" : "");
            ImGui::Text("$EF IRQSTAT: $%02X %s%s", irqStatus, irqStatus & 2 ? "[LINE]" : "", irqStatus & 1 ? "[VBLANK]" : "");
        }
        if (ImGui::CollapsingHeader("Banking")) {
            ImGui::Text("$F0 BANK0: $%02X - page:%u%s%s", bankRegs[0], bankRegs[0] & 0x3F, bankRegs[0] & 0x80 ? " RO" : "", bankRegs[0] & 0x40 ? " OVL" : "");
            ImGui::Text("$F1 BANK1: $%02X - page:%u%s%s", bankRegs[1], bankRegs[1] & 0x3F, bankRegs[1] & 0x80 ? " RO" : "", bankRegs[1] & 0x40 ? " OVL" : "");
            ImGui::Text("$F2 BANK2: $%02X - page:%u%s%s", bankRegs[2], bankRegs[2] & 0x3F, bankRegs[2] & 0x80 ? " RO" : "", bankRegs[2] & 0x40 ? " OVL" : "");
            ImGui::Text("$F3 BANK3: $%02X - page:%u%s%s", bankRegs[3], bankRegs[3] & 0x3F, bankRegs[3] & 0x80 ? " RO" : "", bankRegs[3] & 0x40 ? " OVL" : "");
        }
        if (ImGui::CollapsingHeader("Key buffer")) {
            auto keyMode = Keyboard::instance()->getKeyMode();

            {
                uint8_t val = kbBufCnt == 0 ? 0 : kbBuf[kbBufRdIdx];
                ImGui::Text("$FA KEYBUF: $%02X (%c)", val, val > 32 && val < 127 ? val : '.');
            }
            ImGui::Text(
                "   KEYMODE: $%02X %s%s%s\n",
                keyMode,
                (keyMode & 1) ? "[Enable]" : "",
                (keyMode & 2) ? "[ASCII]" : "[ScanCode]",
                (keyMode & 4) ? "[Repeat]" : "");

            std::string str = "Key buffer: ";

            int rdIdx = kbBufRdIdx;
            for (int i = 0; i < kbBufCnt; i++) {
                if (keyMode & 2) {
                    uint8_t val = kbBuf[rdIdx];
                    str += fmtstr("%c", val > 32 && val < 127 ? val : '.');
                } else {
                    str += fmtstr("%02X ", kbBuf[rdIdx]);
                }
                rdIdx++;
                if (rdIdx == sizeof(kbBuf))
                    rdIdx = 0;
            }
            ImGui::Text("%s", str.c_str());
        }
        if (ImGui::CollapsingHeader("Other")) {
            uint8_t sysctrl =
                ((sysCtrlTurboUnlimited ? (1 << 3) : 0) |
                 (sysCtrlTurbo ? (1 << 2) : 0) |
                 (sysCtrlAyDisable ? (1 << 1) : 0) |
                 (sysCtrlDisableExt ? (1 << 0) : 0));
            ImGui::Text(
                "$FB SYSCTRL: $%02X %s%s%s%s", sysctrl,
                sysctrl & 8 ? "[UNLIMITED]" : "",
                sysctrl & 4 ? "[TURBO]" : "",
                sysctrl & 2 ? "[AYDIS]" : "",
                sysctrl & 1 ? "[EXTDIS]" : "");
        }
        if (ImGui::CollapsingHeader("Audio AY1")) {
            ay1.dbgDrawIoRegs();
        }
        if (ImGui::CollapsingHeader("Audio AY2")) {
            ay2.dbgDrawIoRegs();
        }
        if (ImGui::CollapsingHeader("Sprites")) {
            video.dbgDrawSpriteRegs();
        }
        if (ImGui::CollapsingHeader("Palette")) {
            video.dbgDrawPaletteRegs();
        }
    }
    ImGui::End();
}

void AqpEmuState::dbgWndBreakpoints(bool *p_open) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(330, 132), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Breakpoints", p_open, 0)) {

        ImGui::Checkbox("Enable breakpoints", &enableBreakpoints);
        ImGui::SameLine(ImGui::GetWindowWidth() - 25);
        if (ImGui::Button("+")) {
            breakpoints.emplace_back();
        }
        ImGui::Separator();
        if (ImGui::BeginTable("Table", 8, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupColumn("En", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Symbol", 0);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)breakpoints.size());
            int eraseIdx = -1;

            while (clipper.Step()) {
                for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++) {
                    auto &bp = breakpoints[row_n];

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Checkbox(fmtstr("##en%d", row_n).c_str(), &bp.enabled);
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 6);
                    ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U16, &bp.addr, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AlwaysOverwrite);
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo(fmtstr("##name%d", row_n).c_str(), bp.name.c_str())) {
                        for (auto &sym : asmListing.symbolsStrAddr) {
                            if (ImGui::Selectable(fmtstr("%04X %s", sym.second, sym.first.c_str()).c_str())) {
                                bp.name = sym.first;
                                bp.addr = sym.second;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TableNextColumn();
                    ImGui::Checkbox(fmtstr("##onR%d", row_n).c_str(), &bp.onR);
                    ImGui::TableNextColumn();
                    ImGui::Checkbox(fmtstr("##onW%d", row_n).c_str(), &bp.onW);
                    ImGui::TableNextColumn();
                    ImGui::Checkbox(fmtstr("##onX%d", row_n).c_str(), &bp.onX);
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 9);

                    static const char *types[] = {"Mem", "IO 8", "IO 16"};
                    if (ImGui::BeginCombo(fmtstr("##type%d", row_n).c_str(), types[bp.type])) {
                        for (int i = 0; i < 3; i++) {
                            if (ImGui::Selectable(types[i])) {
                                bp.type = i;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::Button(fmtstr("X##del%d", row_n).c_str())) {
                        eraseIdx = row_n;
                    }
                }
            }
            if (eraseIdx >= 0) {
                breakpoints.erase(breakpoints.begin() + eraseIdx);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void AqpEmuState::addrPopup(uint16_t addr) {
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft) ||
        ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonRight)) {

        auto sym = asmListing.symbolsAddrStr.find(addr);
        if (sym != asmListing.symbolsAddrStr.end()) {
            ImGui::Text("Address $%04X (%s)", addr, sym->second.c_str());
        } else {
            ImGui::Text("Address $%04X", addr);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Run to here")) {
            tmpBreakpoint = addr;
            emuMode       = Em_Running;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Add breakpoint")) {
            Breakpoint bp;
            bp.enabled = true;
            bp.addr    = addr;
            bp.onR     = false;
            bp.onW     = false;
            bp.onX     = true;
            breakpoints.push_back(bp);
            ImGui::CloseCurrentPopup();
            listingReloaded();
        }
        if (ImGui::MenuItem("Show in memory editor")) {
            auto config              = Config::instance();
            config->showMemEdit      = true;
            config->memEditMemSelect = 0;
            memEdit.gotoAddr         = addr;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::BeginMenu("Add watch")) {
            int i = -1;

            if (ImGui::MenuItem("Hex 8"))
                i = (int)WatchType::Hex8;
            if (ImGui::MenuItem("Dec U8"))
                i = (int)WatchType::DecU8;
            if (ImGui::MenuItem("Dec S8"))
                i = (int)WatchType::DecS8;
            if (ImGui::MenuItem("Hex 16"))
                i = (int)WatchType::Hex16;
            if (ImGui::MenuItem("Dec U16"))
                i = (int)WatchType::DecU16;
            if (ImGui::MenuItem("Dec S16"))
                i = (int)WatchType::DecS16;

            if (i >= 0) {
                Watch w;
                w.addr = addr;
                w.type = (WatchType)i;
                watches.push_back(w);
                ImGui::CloseCurrentPopup();
                listingReloaded();
            }

            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
}

void AqpEmuState::dbgWndMemEdit(bool *p_open) {
    struct MemoryArea {
        MemoryArea(const std::string &_name, void *_data, size_t _size)
            : name(_name), data(_data), size(_size) {
        }
        std::string name;
        void       *data;
        size_t      size;
    };
    static std::vector<MemoryArea> memAreas;

    if (memAreas.empty()) {
        memAreas.emplace_back("Z80 memory", nullptr, 0x10000);
        memAreas.emplace_back("Screen RAM", video.screenRam, sizeof(video.screenRam));
        memAreas.emplace_back("Color RAM", video.colorRam, sizeof(video.colorRam));
        memAreas.emplace_back("Page 19: Cartridge ROM", cartRom, sizeof(cartRom));
        memAreas.emplace_back("Page 20: Video RAM", video.videoRam, sizeof(video.videoRam));
        memAreas.emplace_back("Page 21: Character RAM", video.charRam, sizeof(video.charRam));

        for (int i = 32; i < 64; i++) {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "Page %d: Main RAM $%05X-$%05X", i, (i - 32) * 16384, ((i + 1) - 32) * 16384 - 1);
            memAreas.emplace_back(tmp, mainRam + (i - 32) * 16384, 16384);
        }
    }

    auto config = Config::instance();
    if (config->memEditMemSelect < 0 || config->memEditMemSelect > (int)memAreas.size()) {
        // Invalid setting, reset to 0
        config->memEditMemSelect = 0;
    }

    MemoryEditor::Sizes s;
    memEdit.calcSizes(s, memAreas[config->memEditMemSelect].size, 0);
    ImGui::SetNextWindowSize(ImVec2(s.windowWidth, s.windowWidth * 0.60f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(s.windowWidth, 150.0f), ImVec2(s.windowWidth, FLT_MAX));

    if (ImGui::Begin("Memory editor", p_open, ImGuiWindowFlags_NoScrollbar)) {
        if (ImGui::BeginCombo("Memory select", memAreas[config->memEditMemSelect].name.c_str(), ImGuiComboFlags_HeightLargest)) {
            for (int i = 0; i < (int)memAreas.size(); i++) {
                if (ImGui::Selectable(memAreas[i].name.c_str(), config->memEditMemSelect == i)) {
                    config->memEditMemSelect = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        if (config->memEditMemSelect == 0) {
            memEdit.readFn = [this](const ImU8 *data, size_t off) {
                return (ImU8)memRead((uint16_t)off);
            };
            memEdit.writeFn = [this](ImU8 *data, size_t off, ImU8 d) {
                memWrite((uint16_t)off, d);
            };
        } else {
            memEdit.readFn  = nullptr;
            memEdit.writeFn = nullptr;
        }
        memEdit.drawContents(memAreas[config->memEditMemSelect].data, memAreas[config->memEditMemSelect].size, 0);
        if (memEdit.contentsWidthChanged) {
            memEdit.calcSizes(s, memAreas[config->memEditMemSelect].size, 0);
            ImGui::SetWindowSize(ImVec2(s.windowWidth, ImGui::GetWindowSize().y));
        }
    }
    ImGui::End();
}

void AqpEmuState::dbgWndAssemblyListing(bool *p_open) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 200), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Assembly listing", p_open, 0)) {
        if (asmListing.lines.empty()) {
            if (ImGui::Button("Load zmac listing")) {
                char const *lFilterPatterns[1] = {"*.lst"};
                char       *lstFile            = tinyfd_openFileDialog("Open zmac listing file", "", 1, lFilterPatterns, "Zmac listing files", 0);
                if (lstFile) {
                    asmListing.load(lstFile);
                    listingReloaded();
                }
            }
        } else {
            if (ImGui::Button("Reload")) {
                auto path = asmListing.getPath();
                asmListing.load(path);
                listingReloaded();
            }
            ImGui::SameLine();
            if (ImGui::Button("X")) {
                asmListing.clear();
                listingReloaded();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(asmListing.getPath().c_str());
        }
        ImGui::Separator();

        if (ImGui::BeginTable("Table", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
            static float itemsHeight  = -1;
            static int   lastPC       = -1;
            bool         updateScroll = (emuMode == Em_Halted && lastPC != z80ctx.PC);
            if (updateScroll) {
                lastPC = z80ctx.PC;
            }

            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("LineNr", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Text");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)asmListing.lines.size());

            while (clipper.Step()) {
                if (clipper.ItemsHeight > 0) {
                    itemsHeight = clipper.ItemsHeight;
                }

                for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++) {
                    auto &line = asmListing.lines[row_n];

                    ImGui::TableNextRow();
                    if (emuMode == Em_Halted && z80ctx.PC == line.addr) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_TextSelectedBg]));
                        updateScroll = false;
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(line.file.c_str());
                    ImGui::TableNextColumn();
                    if (line.addr >= 0) {
                        char addr[32];
                        snprintf(addr, sizeof(addr), "%04X##%d", line.addr, row_n);
                        ImGui::Selectable(addr);
                        addrPopup(line.addr);
                    }

                    // ImGui::Text("%04X", line.addr);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(line.bytes.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%6d", line.linenr);
                    ImGui::TableNextColumn();

                    {
                        auto commentStart = line.s.find(";");
                        if (commentStart != line.s.npos) {
                            ImGui::TextUnformatted(line.s.substr(0, commentStart).c_str());
                            ImGui::SameLine(0, 0);
                            ImGui::TextColored(ImVec4(0.3f, 0.75f, 0.4f, 1.0f), "%s", line.s.substr(commentStart).c_str());
                        } else {
                            ImGui::TextUnformatted(line.s.c_str());
                        }
                    }
                }
            }

            if (updateScroll) {
                for (unsigned i = 0; i < asmListing.lines.size(); i++) {
                    const auto &line = asmListing.lines[i];

                    if (line.addr >= 0 && line.addr == z80ctx.PC) {
                        ImGui::SetScrollY(std::max(0.0f, itemsHeight * (i - 5)));
                        break;
                    }
                }
            }

            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void AqpEmuState::dbgWndCpuTrace(bool *p_open) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(700, 200), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("CPU trace", p_open, 0)) {
        ImGui::Checkbox("Enable tracing", &traceEnable);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 8);

        const int minDepth = 16, maxDepth = 16384;
        ImGui::DragInt("Trace depth", &traceDepth, 1, minDepth, maxDepth);
        traceDepth = std::max(minDepth, std::min(traceDepth, maxDepth));

        ImGui::Separator();

        if (ImGui::BeginTable("Table", 15, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Instruction", 0);
            ImGui::TableSetupColumn("SP", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("AF", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("BC", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("DE", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("HL", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("IX", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("IY", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("AF'", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("BC'", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("DE'", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("HL'", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)cpuTrace.size());

            auto regColumn = [](uint16_t value, uint16_t prevValue) {
                ImGui::TableNextColumn();

                if (prevValue != value)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32((ImVec4)ImColor(0, 128, 0)));

                ImGui::Text("%04X", value);
            };

            while (clipper.Step()) {
                for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++) {
                    auto &entry     = cpuTrace[row_n];
                    auto &prevEntry = row_n < 1 ? entry : cpuTrace[row_n - 1];

                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%4d", row_n - (traceDepth - 1));
                    ImGui::TableNextColumn();
                    ImGui::Text("%04X", entry.pc);
                    ImGui::TableNextColumn();
                    ImGui::Text("%-11s", entry.bytes + 1);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(entry.instrStr);

                    regColumn(entry.r1.wr.SP, prevEntry.r1.wr.SP);
                    regColumn(entry.r1.wr.AF, prevEntry.r1.wr.AF);
                    regColumn(entry.r1.wr.BC, prevEntry.r1.wr.BC);
                    regColumn(entry.r1.wr.DE, prevEntry.r1.wr.DE);
                    regColumn(entry.r1.wr.HL, prevEntry.r1.wr.HL);
                    regColumn(entry.r1.wr.IX, prevEntry.r1.wr.IX);
                    regColumn(entry.r1.wr.IY, prevEntry.r1.wr.IY);
                    regColumn(entry.r2.wr.AF, prevEntry.r2.wr.AF);
                    regColumn(entry.r2.wr.BC, prevEntry.r2.wr.BC);
                    regColumn(entry.r2.wr.DE, prevEntry.r2.wr.DE);
                    regColumn(entry.r2.wr.HL, prevEntry.r2.wr.HL);
                    // ImGui::TextUnformatted(line.file.c_str());
                }
            }

            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void AqpEmuState::dbgWndWatch(bool *p_open) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(330, 132), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Watch", p_open, 0)) {
        if (ImGui::BeginTable("Table", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Symbol", 0);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            int              numWatches = (int)watches.size();
            clipper.Begin(numWatches + 1);
            int eraseIdx = -1;

            while (clipper.Step()) {
                for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++) {
                    ImGui::TableNextRow();

                    if (row_n < numWatches) {
                        auto &w = watches[row_n];
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 7);
                        switch (w.type) {
                            case WatchType::Hex8: {
                                uint8_t val = memRead(w.addr);
                                if (ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U8, &val, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                                    memWrite(w.addr, val);
                                }
                                break;
                            }
                            case WatchType::DecU8: {
                                uint8_t val = memRead(w.addr);
                                if (ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U8, &val, nullptr, nullptr, "%u", ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                                    memWrite(w.addr, val);
                                }
                                break;
                            }
                            case WatchType::DecS8: {
                                int8_t val = memRead(w.addr);
                                if (ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_S8, &val, nullptr, nullptr, "%d", ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                                    memWrite(w.addr, val);
                                }
                                break;
                            }
                            case WatchType::Hex16: {
                                uint16_t val = memRead(w.addr) | (memRead(w.addr + 1) << 8);
                                if (ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U16, &val, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                                    memWrite(w.addr, val & 0xFF);
                                    memWrite(w.addr + 1, (val >> 8) & 0xFF);
                                }
                                break;
                            }
                            case WatchType::DecU16: {
                                uint16_t val = memRead(w.addr) | (memRead(w.addr + 1) << 8);
                                if (ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U16, &val, nullptr, nullptr, "%u", ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                                    memWrite(w.addr, val & 0xFF);
                                    memWrite(w.addr + 1, (val >> 8) & 0xFF);
                                }
                                break;
                            }
                            case WatchType::DecS16: {
                                int16_t val = memRead(w.addr) | (memRead(w.addr + 1) << 8);
                                if (ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_S16, &val, nullptr, nullptr, "%d", ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                                    memWrite(w.addr, val & 0xFF);
                                    memWrite(w.addr + 1, (val >> 8) & 0xFF);
                                }
                                break;
                            }
                        }
                        // ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U16, &w.value, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AlwaysOverwrite);
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 6);
                        if (ImGui::InputScalar(fmtstr("##addr%d", row_n).c_str(), ImGuiDataType_U16, &w.addr, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AlwaysOverwrite)) {
                            auto sym = asmListing.symbolsAddrStr.find(w.addr);
                            if (sym != asmListing.symbolsAddrStr.end()) {
                                w.name = sym->second;
                            } else {
                                w.name = "";
                            }
                        }
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo(fmtstr("##name%d", row_n).c_str(), w.name.c_str())) {
                            for (auto &sym : asmListing.symbolsStrAddr) {
                                if (ImGui::Selectable(fmtstr("%04X %s", sym.second, sym.first.c_str()).c_str())) {
                                    w.name = sym.first;
                                    w.addr = sym.second;
                                }
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 11);

                        static const char *types[] = {"Hex 8", "Dec U8", "Dec S8", "Hex 16", "Dec U16", "Dec S16"};
                        if (ImGui::BeginCombo(fmtstr("##type%d", row_n).c_str(), types[(int)w.type])) {
                            for (int i = 0; i < 6; i++) {
                                if (ImGui::Selectable(types[i])) {
                                    w.type = (WatchType)i;
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::TableNextColumn();
                        if (ImGui::Button(fmtstr("X##del%d", row_n).c_str())) {
                            eraseIdx = row_n;
                        }
                    } else {
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                        if (ImGui::Button(fmtstr("+##add%d", row_n).c_str())) {
                            watches.emplace_back();
                        }
                    }
                }
            }
            if (eraseIdx >= 0) {
                watches.erase(watches.begin() + eraseIdx);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void AqpEmuState::listingReloaded() {
    // Update watches
    for (auto &w : watches) {
        if (!asmListing.findSymbolAddr(w.name, w.addr)) {
            if (!asmListing.findSymbolName(w.addr, w.name)) {
                w.name = "";
            }
        }
    }

    // Update breakpoints
    for (auto &bp : breakpoints) {
        if (!asmListing.findSymbolAddr(bp.name, bp.addr)) {
            if (!asmListing.findSymbolName(bp.addr, bp.name)) {
                bp.name = "";
            }
        }
    }
}
