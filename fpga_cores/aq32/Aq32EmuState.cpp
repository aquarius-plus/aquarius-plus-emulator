#include "EmuState.h"
#include "DCBlock.h"
#include "UartProtocol.h"
#include "FPGA.h"
#include "cpu/riscv.h"
#include "Config.h"
#include "bootrom.h"
#include "Aq32Video.h"
#include "imgui.h"
#include "Keyboard.h"

#ifndef WIN32
#define GDB_ENABLE
#endif

#ifdef GDB_ENABLE
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
#include "MemoryEditor.h"

#define HCYCLES_PER_LINE   (455)
#define HCYCLES_PER_SAMPLE (162)

#define BASE_BOOTROM 0x00000
#define REG_ESPCTRL  0x02000
#define REG_ESPDATA  0x02004
#define REG_VCTRL    0x02008
#define REG_VSCRX    0x0200C
#define REG_VSCRY    0x02010
#define REG_VLINE    0x02014
#define REG_VIRQLINE 0x02018
#define REG_KEYBUF   0x0201C
#define BASE_SPRATTR 0x03000
#define BASE_PALETTE 0x04000
#define BASE_CHRAM   0x05000
#define BASE_TEXTRAM 0x06000
#define BASE_VRAM    0x08000
#define BASE_MAINRAM 0x80000

#ifdef GDB_ENABLE
static std::string bufToHex(const void *buf, unsigned size) {
    std::string result;

    auto p = reinterpret_cast<const uint8_t *>(buf);
    while (size--)
        result += fmtstr("%02X", *(p++));

    return result;
}

static int64_t getTimeUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

class Aq32EmuState : public EmuState {
public:
    riscv               cpu;
    Aq32Video           video;
    uint8_t             keybMatrix[8] = {0};
    std::deque<uint8_t> kbBuf;
    const unsigned      kbBufSize  = 16;
    unsigned            audioLeft  = 0;
    unsigned            audioRight = 0;
    DCBlock             dcBlockLeft;
    DCBlock             dcBlockRight;
    std::string         typeInStr;
    std::mutex          mutexTypeInStr;
    uint32_t            mainRam[512 * 1024 / 4];
    uint32_t            bootRom[0x800 / 4];

#ifdef GDB_ENABLE
    // GDB interface
    int         listenSocket = -1;
    int         conn         = -1;
    int         rxState      = 0;
    std::string rxStr;
    char        rxChecksum[3];
#endif

    enum EmuMode {
        Em_Halted,
        Em_Step,
        Em_Running,
    };
    EmuMode emuMode = Em_Running;

    bool showCpuState      = false;
    bool showBreakpoints   = false;
    bool showIoRegsWindow  = false;
    bool showMemEdit       = false;
    int  memEditMemSelect  = 0;
    bool enableBreakpoints = false;

    struct Breakpoint {
        uint32_t    addr = 0;
        std::string name;
        bool        enabled = false;
    };

    std::vector<Breakpoint> breakpoints;

    MemoryEditor memEdit;

    Aq32EmuState() {
        coreType         = 1;
        coreFlags        = 0x02;
        coreVersionMajor = 0;
        coreVersionMinor = 1;
        memcpy(coreName, "Aquarius32      ", sizeof(coreName));

        cpu.dataWrite = [this](uint32_t vaddr, uint32_t val, uint32_t mask) { memWrite(vaddr, val, mask); };
        cpu.dataRead  = [this](uint32_t vaddr) { auto val = memRead(vaddr); if (val < 0) val = 0; return (uint32_t)val; };
        cpu.instrRead = [this](uint32_t vaddr) { auto val = memRead(vaddr); if (val < 0) val = 0; return (uint32_t)val; };

        memset(keybMatrix, 0xFF, sizeof(keybMatrix));
        memcpy(bootRom, bootrom_bin, bootrom_bin_len);
        loadConfig();
        reset();
    }

    virtual ~Aq32EmuState() {
        saveConfig();
    }

    void reset(bool cold = false) override {
        // CPU reset
        cpu.regs[0]      = 0;
        cpu.pc           = 0;
        cpu.mstatus_mie  = false;
        cpu.mstatus_mpie = false;
        cpu.mie          = 0;
        cpu.mtvec        = 0;
        cpu.mscratch     = 0;
        cpu.mepc         = 0;
        cpu.mcause       = 0;
        cpu.mtval        = 0;
        cpu.mip          = 0;
        cpu.trap         = 0;

        kbBuf.clear();
    }

    void loadConfig() {
        auto root   = Config::instance()->loadConfigFile("aq32.json");
        showMemEdit = getBoolValue(root, "showMemEdit", false);
        // memEditMemSelect = getIntValue(root, "memEditMemSelect", 0);
        showCpuState     = getBoolValue(root, "showCpuState", false);
        showBreakpoints  = getBoolValue(root, "showBreakpoints", false);
        showIoRegsWindow = getBoolValue(root, "showIoRegsWindow", false);

        cJSON_Delete(root);
    }

    void saveConfig() {
        auto root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "showMemEdit", showMemEdit);
        // cJSON_AddNumberToObject(root, "memEditMemSelect", memEditMemSelect);
        cJSON_AddBoolToObject(root, "showCpuState", showCpuState);
        cJSON_AddBoolToObject(root, "showBreakpoints", showBreakpoints);
        cJSON_AddBoolToObject(root, "showIoRegsWindow", showIoRegsWindow);

        Config::instance()->saveConfigFile("aq32.json", root);
    }

    void getVideoSize(int &w, int &h) override {
        w = 640;
        h = 480;
    }

    void getPixels(void *pixels, int pitch) override {
        const uint16_t *fb = video.getFb();

        bool videoMode    = true;
        auto activeWidth  = Aq32Video::activeWidth;
        auto activeHeight = Aq32Video::activeHeight;
        int  w            = videoMode ? 640 : Aq32Video::activeWidth;

        for (int j = 0; j < activeHeight * 2; j++) {
            for (int i = 0; i < w; i++) {
                ((uint32_t *)((uintptr_t)pixels + j * pitch))[i] =
                    col12_to_col32(fb[(j / 2) * activeWidth + (videoMode ? (i + 32) : i)]);
            }
        }
        renderOverlay(pixels, pitch);
    }

    void spiTx(const void *data, size_t length) override {
        EmuState::spiTx(data, length);
        if (!spiSelected || txBuf.empty())
            return;

        switch (txBuf[0]) {
            case CMD_SET_KEYB_MATRIX: {
                if (txBuf.size() == 1 + 8) {
                    memcpy(&keybMatrix, &txBuf[1], 8);
                }
                break;
            }

            case CMD_WRITE_KBBUF: {
                if (txBuf.size() == 1 + 1 && kbBuf.size() < kbBufSize) {
                    kbBuf.push_back(txBuf[1]);
                }
                break;
            }
        }
    }

    void pasteText(const std::string &str) override {
        std::lock_guard lock(mutexTypeInStr);
        typeInStr = str;
    }
    bool pasteIsDone() override {
        std::lock_guard lock(mutexTypeInStr);
        return typeInStr.empty();
    }

    int64_t memRead(uint32_t addr, bool allow_side_effect = true) {
        if (/* addr >= BASE_BOOTROM && */ addr < (BASE_BOOTROM + sizeof(bootRom))) {
            return bootRom[(addr & 0x7FF) / 4];
        } else if (addr == REG_ESPCTRL) {
            if (allow_side_effect)
                return UartProtocol::instance()->readCtrl();
            else
                return 0;
        } else if (addr == REG_ESPDATA) {
            if (allow_side_effect)
                return UartProtocol::instance()->readData();
            else
                return 0;
        } else if (addr == REG_VCTRL) {
            return video.videoCtrl;
        } else if (addr == REG_VSCRX) {
            return video.videoScrX;
        } else if (addr == REG_VSCRY) {
            return video.videoScrY;
        } else if (addr == REG_VLINE) {
            return video.videoLine;
        } else if (addr == REG_VIRQLINE) {
            return video.videoIrqLine;
        } else if (addr == REG_KEYBUF) {
            uint8_t result = 0;
            if (!kbBuf.empty()) {
                result = kbBuf.front();
                if (allow_side_effect)
                    kbBuf.pop_front();
            }
            return result;
        } else if (addr >= BASE_SPRATTR && addr < (BASE_SPRATTR + 64 * 8)) {
            // Sprite attributes (32b)
            int sprIdx = (addr >> 3) & 63;
            if (addr & 4) {
                return video.sprites[sprIdx].attr;
            } else {
                return (video.sprites[sprIdx].y << 16) | (video.sprites[sprIdx].x & 0x1FF);
            }
        } else if (addr >= BASE_PALETTE && addr < (BASE_PALETTE + sizeof(video.videoPalette))) {
            // Palette (16b)
            return video.videoPalette[(addr & (sizeof(video.videoPalette) - 1)) / 2];
        } else if (addr >= BASE_CHRAM && addr < (BASE_CHRAM + sizeof(video.charRam))) {
            // Character RAM (8b)
            return video.charRam[addr & (sizeof(video.charRam) - 1)];
        } else if (addr >= BASE_TEXTRAM && addr < (BASE_TEXTRAM + sizeof(video.textRam))) {
            // Text RAM (8b/16b)
            uint32_t val = video.textRam[(addr & (sizeof(video.textRam) - 1)) / 2];
            val |= val << 16;
            return val;
        } else if (addr >= BASE_VRAM && addr < (BASE_VRAM + sizeof(video.videoRam))) {
            // Video RAM (8/16/32b)
            return reinterpret_cast<uint32_t *>(video.videoRam)[addr & (sizeof(video.videoRam) - 1)];
        } else if (addr >= BASE_MAINRAM && addr < (BASE_MAINRAM + sizeof(mainRam))) {
            return mainRam[(addr & (sizeof(mainRam) - 1)) / 4];
        }
        return -1;
    }

    void memWrite(uint32_t addr, uint32_t val, uint32_t mask) {
        if (addr == REG_ESPCTRL) {
            UartProtocol::instance()->writeCtrl(val & 0xFF);
        } else if (addr == REG_ESPDATA) {
            if (val & 0x100) {
                UartProtocol::instance()->writeCtrl(0x80);
            } else {
                UartProtocol::instance()->writeData(val & 0xFF);
            }
        } else if (addr == REG_VCTRL) {
            video.videoCtrl = val & 0xFF;
        } else if (addr == REG_VSCRX) {
            video.videoScrX = val & 0x1FF;
        } else if (addr == REG_VSCRY) {
            video.videoScrY = val & 0xFF;
        } else if (addr == REG_VLINE) {
        } else if (addr == REG_VIRQLINE) {
            video.videoIrqLine = val & 0xFF;
        } else if (addr == REG_KEYBUF) {
            kbBuf.clear();
        } else if (addr >= BASE_SPRATTR && addr < (BASE_SPRATTR + 64 * 8)) {
            // Sprite attributes (32b)
            int sprIdx = (addr >> 3) & 63;
            if (addr & 4) {
                video.sprites[sprIdx].attr = val & 0xFFFF;
            } else {
                video.sprites[sprIdx].x = val & 0x1FF;
                video.sprites[sprIdx].y = (val >> 16) & 0xFF;
            }
        } else if (addr >= BASE_PALETTE && addr < (BASE_PALETTE + sizeof(video.videoPalette))) {
            // Palette (16b)
            video.videoPalette[(addr & (sizeof(video.videoPalette) - 1)) / 2] = val & 0xFFF;
        } else if (addr >= BASE_CHRAM && addr < (BASE_CHRAM + sizeof(video.charRam))) {
            // Character RAM (8b)
            video.charRam[addr & (sizeof(video.charRam) - 1)] = val & 0xFF;
        } else if (addr >= BASE_TEXTRAM && addr < (BASE_TEXTRAM + sizeof(video.textRam))) {
            // Text RAM (8b/16b)
            uint16_t msk = (mask >> 16) | (mask & 0xFFFF);
            auto     p   = &video.textRam[(addr & (sizeof(video.textRam) - 1)) / 2];
            *p           = (*p & ~msk) | (val & msk);
        } else if (addr >= BASE_VRAM && addr < (BASE_VRAM + sizeof(video.videoRam))) {
            // Video RAM (8/16/32b)
            auto p = reinterpret_cast<uint32_t *>(&video.videoRam[addr & (sizeof(video.videoRam) - 1)]);
            *p     = (*p & ~mask) | (val & mask);
        } else if (addr >= BASE_MAINRAM && addr < (BASE_MAINRAM + sizeof(mainRam))) {
            auto p = &mainRam[(addr & (sizeof(mainRam) - 1)) / 4];
            *p     = (*p & ~mask) | (val & mask);
        }
    }

    void emulateFrame(int16_t *audioBuf, unsigned numSamples) override {
        for (int line = 0; line < 262; line++) {
            video.videoLine = line;
            int count       = 0;
            if (emuMode == Em_Running)
                count = 8000000 / 60 / 262;
            else if (emuMode == Em_Step)
                count = 1;

#ifdef GDB_ENABLE
            if (enableDebugger)
                gdbProcess();
#endif

            while (count--) {
                cpu.emulate();

                if (enableDebugger && enableBreakpoints) {
                    for (auto &bp : breakpoints) {
                        if (bp.enabled && bp.addr == cpu.pc) {
                            emuMode = Em_Halted;
#ifdef GDB_ENABLE
                            gdbBreakpointHit();
#endif
                            return;
                        }
                    }
                }
            }

            if (emuMode == Em_Step) {
                emuMode = Em_Halted;
            }
            video.drawLine(line);
            keyboardTypeIn();
        }
    }

    void keyboardTypeIn() {
        std::lock_guard lock(mutexTypeInStr);
        if (kbBuf.size() < kbBufSize && !typeInStr.empty()) {
            char ch = typeInStr.front();
            typeInStr.erase(typeInStr.begin());
            Keyboard::instance()->pressKey(ch);
        }
    }

    void dbgMenu() override {
        if (!enableDebugger)
            return;

        ImGui::MenuItem("CPU state", "", &showCpuState);
        ImGui::MenuItem("Breakpoints", "", &showBreakpoints);
        ImGui::MenuItem("Memory editor", "", &showMemEdit);
        ImGui::MenuItem("IO Registers", "", &showIoRegsWindow);
    }

    void dbgWindows() override {
        if (!enableDebugger)
            return;

        if (showCpuState)
            dbgWndCpuState(&showCpuState);
        if (showBreakpoints)
            dbgWndBreakpoints(&showBreakpoints);

        if (showMemEdit)
            dbgWndMemEdit(&showMemEdit);
        if (showIoRegsWindow)
            dbgWndIoRegs(&showIoRegsWindow);
    }

    void dbgWndIoRegs(bool *p_open) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(330, 132), ImVec2(330, FLT_MAX));
        if (ImGui::Begin("IO Registers", p_open, 0)) {
            if (ImGui::CollapsingHeader("Video")) {
                video.dbgDrawIoRegs();
            }
            if (ImGui::CollapsingHeader("Sprites")) {
                video.dbgDrawSpriteRegs();
            }
            if (ImGui::CollapsingHeader("Palette")) {
                video.dbgDrawPaletteRegs();
            }
            if (ImGui::CollapsingHeader("Key buffer")) {
                auto keyMode = Keyboard::instance()->getKeyMode();

                {
                    uint8_t val = kbBuf.empty() ? 0 : kbBuf.front();
                    ImGui::Text("$FA KEYBUF: $%02X (%c)", val, val > 32 && val < 127 ? val : '.');
                }
                ImGui::Text(
                    "   KEYMODE: $%02X %s%s%s\n",
                    keyMode,
                    (keyMode & 1) ? "[Enable]" : "",
                    (keyMode & 2) ? "[ASCII]" : "[ScanCode]",
                    (keyMode & 4) ? "[Repeat]" : "");

                std::string str = "Key buffer: ";

                for (unsigned i = 0; i < kbBuf.size(); i++) {
                    if (keyMode & 2) {
                        uint8_t val = kbBuf[i];
                        str += fmtstr("%c", val > 32 && val < 127 ? val : '.');
                    } else {
                        str += fmtstr("%02X ", kbBuf[i]);
                    }
                }
                ImGui::Text("%s", str.c_str());
            }
        }
        ImGui::End();
    }

    void dbgWndCpuState(bool *p_open) {
        bool open = ImGui::Begin("CPU state", p_open, ImGuiWindowFlags_AlwaysAutoResize);
        if (open) {
            ImGui::PushStyleColor(ImGuiCol_Button, emuMode == Em_Halted ? (ImVec4)ImColor(192, 0, 0) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
            ImGui::BeginDisabled(emuMode != Em_Running);
            if (ImGui::Button("Halt")) {
                emuMode = Em_Halted;
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor();

            ImGui::BeginDisabled(emuMode == Em_Running);
            ImGui::SameLine();
            if (ImGui::Button("Step Into")) {
                emuMode = Em_Step;
            }
#if 0
            ImGui::SameLine();
            if (ImGui::Button("Step Over")) {
                // int tmpBreakpoint = -1;
    
                // if (emuState.z80ctx.halted) {
                //     // Step over HALT instruction
                //     emuState.z80ctx.halted = 0;
                //     emuState.z80ctx.PC++;
                // } else {
                //     uint8_t opcode = emuState.memRead(emuState.z80ctx.PC);
                //     if (opcode == 0xCD ||          // CALL nn
                //         (opcode & 0xC7) == 0xC4) { // CALL c,nn
    
                //         tmpBreakpoint = emuState.z80ctx.PC + 3;
    
                //     } else if ((opcode & 0xC7) == 0xC7) { // RST
                //         tmpBreakpoint = emuState.z80ctx.PC + 1;
                //         if ((opcode & 0x38) == 0x08 ||
                //             (opcode & 0x38) == 0x30) {
    
                //             // Skip one extra byte on RST 08H/30H, since on the Aq these
                //             // system calls absorb the byte following this instruction.
                //             tmpBreakpoint++;
                //         }
    
                //     } else if (opcode == 0xED) {
                //         opcode = emuState.memRead(emuState.z80ctx.PC + 1);
                //         if (opcode == 0xB9 || // CPDR
                //             opcode == 0xB1 || // CPIR
                //             opcode == 0xBA || // INDR
                //             opcode == 0xB2 || // INIR
                //             opcode == 0xB8 || // LDDR
                //             opcode == 0xB0 || // LDIR
                //             opcode == 0xBB || // OTDR
                //             opcode == 0xB3) { // OTIR
                //         }
                //         tmpBreakpoint = emuState.z80ctx.PC + 2;
                //     }
                //     emuState.tmpBreakpoint = tmpBreakpoint;
                //     if (tmpBreakpoint >= 0) {
                //         emuMode = Em_Running;
                //     } else {
                //         emuMode = Em_Step;
                //     }
                // }
            }
    
            ImGui::SameLine();
            if (ImGui::Button("Step Out")) {
                emuState.haltAfterRet = 0;
                emuMode      = Em_Running;
            }
#endif

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, emuMode == Em_Running ? (ImVec4)ImColor(0, 128, 0) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
            if (ImGui::Button("Go")) {
                emuMode = Em_Running;
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();

            ImGui::Separator();

            // {
            //     uint16_t    addr = emuState.z80ctx.PC;
            //     std::string name;
            //     if (asmListing.findNearestSymbol(addr, name)) {
            //         ImGui::Text("%s ($%04X + %u)", name.c_str(), addr, emuState.z80ctx.PC - addr);
            //         ImGui::Separator();
            //     }
            // }

            // {
            //     char tmp1[64];
            //     char tmp2[64];
            //     emuState.z80ctx.tstates = 0;

            //     bool prevEnableBp          = emuState.enableBreakpoints;
            //     emuState.enableBreakpoints = false;
            //     Z80Debug(&emuState.z80ctx, tmp1, tmp2);
            //     emuState.enableBreakpoints = prevEnableBp;

            //     ImGui::Text("         %-12s %s", tmp1, tmp2);
            // }

            {
                auto data = memRead(cpu.pc, false);
                auto str  = instrToString((uint32_t)data, cpu.pc);
                ImGui::Text("%08X %s", (unsigned)data, str.c_str());
            }

            ImGui::Separator();

            auto drawReg = [&](const std::string &name, uint32_t val) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%-3s", name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", val);
            };

            if (ImGui::BeginTable("RegTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                static const char *regs[] = {
                    "x0/zero", "x1/ra", "x2/sp", "x3/gp",
                    "x4/tp", "x5/t0", "x6/t1", "x7/t2",
                    "x8/s0/fp", "x9/s1", "x10/a0", "x11/a1",
                    "x12/a2", "x13/a3", "x14/a4", "x15/a5",
                    "x16/a6", "x17/a7", "x18/s2", "x19/s3",
                    "x20/s4", "x21/s5", "x22/s6", "x23/s7",
                    "x24/s8", "x25/s9", "x26/s10", "x27/s11",
                    "x28/t3", "x29/t4", "x30/t5", "x31/t6"};
                drawReg("pc", cpu.pc);

                for (int i = 1; i < 32; i++) {
                    drawReg(regs[i], cpu.regs[i]);
                }

                uint32_t mstatus = 0;
                {
                    if (cpu.mstatus_mie)
                        mstatus |= (1 << 3);
                    if (cpu.mstatus_mpie)
                        mstatus |= (1 << 7);
                }

                drawReg("mstatus", mstatus);
                drawReg("mie", cpu.mie);
                drawReg("mtvec", cpu.mtvec);
                drawReg("mscratch", cpu.mscratch);
                drawReg("mepc", cpu.mepc);
                drawReg("mcause", cpu.mcause);
                drawReg("mtval", cpu.mtval);
                drawReg("mip", cpu.mip);

                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    void dbgWndBreakpoints(bool *p_open) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(330, 132), ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::Begin("Breakpoints", p_open, 0)) {
            ImGui::Checkbox("Enable breakpoints", &enableBreakpoints);
            ImGui::SameLine(ImGui::GetWindowWidth() - 25);
            if (ImGui::Button("+")) {
                breakpoints.emplace_back();
            }
            ImGui::Separator();
            if (ImGui::BeginTable("Table", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
                ImGui::TableSetupColumn("En", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Symbol", 0);
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
                        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F").x * 10);
                        ImGui::InputScalar(fmtstr("##val%d", row_n).c_str(), ImGuiDataType_U32, &bp.addr, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AlwaysOverwrite);
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo(fmtstr("##name%d", row_n).c_str(), bp.name.c_str())) {
                            //     for (auto &sym : asmListing.symbolsStrAddr) {
                            //         if (ImGui::Selectable(fmtstr("%04X %s", sym.second, sym.first.c_str()).c_str())) {
                            //             bp.name  = sym.first;
                            //             bp.value = sym.second;
                            //         }
                            //     }
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

    void dbgWndMemEdit(bool *p_open) {
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
            memAreas.emplace_back("Memory", nullptr, 0x100000);
            memAreas.emplace_back("Text RAM", video.textRam, sizeof(video.textRam));
        }

        if (memEditMemSelect < 0 || memEditMemSelect > (int)memAreas.size()) {
            // Invalid setting, reset to 0
            memEditMemSelect = 0;
        }

        MemoryEditor::Sizes s;
        memEdit.calcSizes(s, memAreas[memEditMemSelect].size, 0);
        ImGui::SetNextWindowSize(ImVec2(s.windowWidth, s.windowWidth * 0.60f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(s.windowWidth, 150.0f), ImVec2(s.windowWidth, FLT_MAX));

        if (ImGui::Begin("Memory editor", p_open, ImGuiWindowFlags_NoScrollbar)) {
            if (ImGui::BeginCombo("Memory select", memAreas[memEditMemSelect].name.c_str(), ImGuiComboFlags_HeightLargest)) {
                for (int i = 0; i < (int)memAreas.size(); i++) {
                    if (ImGui::Selectable(memAreas[i].name.c_str(), memEditMemSelect == i)) {
                        memEditMemSelect = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();

            if (memEditMemSelect == 0) {
                memEdit.readFn = [this](const ImU8 *data, size_t off) {
                    auto val = memRead((uint32_t)off, false);
                    if (val < 0)
                        return -1;

                    return (int)(uint8_t)((val >> ((off & 3) * 8)) & 0xFF);
                };
                memEdit.writeFn = [this](ImU8 *data, size_t off, ImU8 d) {
                    // memWrite((uint16_t)off, d);
                };
            } else {
                memEdit.readFn  = nullptr;
                memEdit.writeFn = nullptr;
            }
            memEdit.drawContents(memAreas[memEditMemSelect].data, memAreas[memEditMemSelect].size, 0);
            if (memEdit.contentsWidthChanged) {
                memEdit.calcSizes(s, memAreas[memEditMemSelect].size, 0);
                ImGui::SetWindowSize(ImVec2(s.windowWidth, ImGui::GetWindowSize().y));
            }
        }
        ImGui::End();
    }

#ifdef GDB_ENABLE
    void gdbProcess() {
        bool gotCommand = false;
        auto tStart     = getTimeUs();

        while (1) {
            if (listenSocket < 0) {
                listenSocket = socket(AF_INET, SOCK_STREAM, 0);

                const int enable = 1;
                setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

                fcntl(listenSocket, F_SETFL, fcntl(listenSocket, F_GETFL, 0) | O_NONBLOCK);

                sockaddr_in serverAddress;
                serverAddress.sin_family      = AF_INET;
                serverAddress.sin_port        = htons(2331);
                serverAddress.sin_addr.s_addr = INADDR_ANY;
                bind(listenSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
                listen(listenSocket, 5);
            }

            if (conn < 0) {
                conn = accept(listenSocket, nullptr, nullptr);
                if (conn < 0)
                    return;

                fcntl(conn, F_SETFL, fcntl(conn, F_GETFL, 0) | O_NONBLOCK);
                // dprintf("Got connection!\n");

                reset();
                emuMode = Em_Halted;
                breakpoints.clear();
            }

            if (conn >= 0) {
                uint8_t buf[512];
                int     sz = recv(conn, buf, sizeof(buf), 0);
                if (sz <= 0) {
                    if (sz == -1 && errno == EAGAIN) {
                        // No data received
                        if (emuMode != Em_Running) {
                            if (gotCommand && (getTimeUs() - tStart) < 1000000)
                                continue;
                        }
                        return;
                    }

                    close(conn);
                    conn = -1;
                    // dprintf("Connection closed.\n");
                    exit(0);
                    return;
                }

                gotCommand = true;
                gdbParseBuf(buf, sz);
            }
        }
    }

    void gdbParseBuf(const uint8_t *p, size_t len) {
        while (len--) {
            uint8_t ch = *(p++);

            if (ch == 3) {
                // CTRL-C
                // dprintf("Halting\n");
                emuMode = Em_Halted;

                send(conn, "+", 1, 0);
                // last signal
                gdbSendStopReply();

            } else if (ch == '$') {
                rxState = 1;
                rxStr.clear();
            } else if (rxState == 1) {
                if (ch == '#') {
                    rxState = 2;
                } else {
                    rxStr += ch;
                }
            } else if (rxState == 2) {
                rxChecksum[0] = ch;
                rxState       = 3;
            } else if (rxState == 3) {
                rxChecksum[1] = ch;
                rxChecksum[2] = 0;

                uint8_t checksum = strtoul(rxChecksum, nullptr, 16);

                uint8_t checksum2 = 0;
                for (uint8_t ch : rxStr) {
                    checksum2 += ch;
                }
                if (checksum2 == checksum) {
                    // dprintf("> '%s'\n", rxStr.c_str());
                    // dprintf("< +\n");
                    send(conn, "+", 1, 0);
                    gdbReceivedCmd(rxStr);
                } else {
                    // dprintf("< -\n");
                    send(conn, "-", 1, 0);
                }

                rxState = 0;
                rxStr.clear();
            }
        }
    }

    void gdbBreakpointHit() {
        if (conn >= 0)
            gdbSendStopReply();
    }

    std::string gdbReadMemory(uintptr_t addr, size_t size) {
        // dprintf("Read memory %08lX (size:%08lX)\n", addr, size);

        std::vector<uint8_t> respData;

        while (size) {
            auto data = memRead(addr, false);
            if (data < 0)
                break;

            if ((addr & 3) == 0) {
                respData.push_back((data >> 0) & 0xFF);
                if (size >= 2)
                    respData.push_back((data >> 8) & 0xFF);
                if (size >= 3)
                    respData.push_back((data >> 16) & 0xFF);
                if (size >= 4)
                    respData.push_back((data >> 24) & 0xFF);

                if (size <= 4)
                    break;

                size -= 4;
                addr += 4;

            } else if ((addr & 3) == 1) {
                respData.push_back((data >> 8) & 0xFF);
                size--;
                addr++;
            } else if ((addr & 3) == 2) {
                respData.push_back((data >> 16) & 0xFF);
                size--;
                addr++;
            } else if ((addr & 3) == 3) {
                respData.push_back((data >> 24) & 0xFF);
                size--;
                addr++;
            }
        }
        return bufToHex(respData.data(), respData.size());
    }

    void gdbWriteMemory(uintptr_t addr, std::vector<uint8_t> data) {
        if (addr >= BASE_MAINRAM && addr < (BASE_MAINRAM + sizeof(mainRam))) {
            addr -= BASE_MAINRAM;

            unsigned endAddr = addr + data.size();
            if (endAddr > BASE_MAINRAM + sizeof(mainRam))
                endAddr = BASE_MAINRAM + sizeof(mainRam);

            memcpy(((uint8_t *)mainRam) + addr, data.data(), endAddr - addr);

        } else {
            printf("Write memory %08lX: ", addr);
            for (auto val : data) {
                printf("%02X ", val);
            }
            printf("\n");
            exit(1);
        }
    }

    void gdbReceivedCmd(const std::string &cmd) {
        if (cmd.empty())
            return;

        if (cmd == "?") {
            // last signal
            gdbSendStopReply();

        } else if (cmd == "qAttached") {
            gdbSendResponse("1");
        } else if (cmd == "qTStatus") {
            gdbSendResponse("T0");

            // } else if (cmd == "vCont?") {
            //     gdbSendResponse("vCont;c;s;t");

        } else if (cmd == "Hg0") {
            gdbSendResponse("OK");
        } else if (cmd == "Hc-1") {
            gdbSendResponse("OK");
        } else if (cmd == "qfThreadInfo") {
            gdbSendResponse("m00");
        } else if (cmd == "qsThreadInfo") {
            gdbSendResponse("l");
        } else if (cmd == "qC") {
            gdbSendResponse("QC0000");
        } else if (cmd == "qOffsets") {
            gdbSendResponse("Text=00;Data=00;Bss=00");
        } else if (cmd == "qSymbol") {
            gdbSendResponse("OK");

        } else if (cmd == "g") {
            // read registers
            uint32_t regs[33];
            for (int i = 0; i < 32; i++) {
                regs[i] = cpu.regs[i];
            }
            regs[32] = cpu.pc;
            gdbSendResponse(bufToHex(regs, sizeof(regs)));

        } else if (cmd[0] == 'm') {
            char    *endptr;
            unsigned addr = strtoul(cmd.c_str() + 1, &endptr, 16);
            if (endptr[0] == ',') {
                unsigned size = strtoul(endptr + 1, &endptr, 16);
                gdbSendResponse(gdbReadMemory(addr, size));
            }

        } else if (cmd[0] == 'M') {
            char    *endptr;
            unsigned addr = strtoul(cmd.c_str() + 1, &endptr, 16);
            if (endptr[0] == ',') {
                unsigned size = strtoul(endptr + 1, &endptr, 16);

                if (endptr[0] == ':') {
                    std::vector<uint8_t> data;
                    const char          *p = endptr + 1;

                    for (unsigned i = 0; i < size; i++) {
                        uint8_t val = 0;

                        for (int j = 0; j < 2; j++) {
                            uint8_t ch = *(p++);
                            if (ch >= '0' && ch <= '9') {
                                val = (val << 4) | (ch - '0');
                            } else if (ch >= 'a' && ch <= 'f') {
                                val = (val << 4) | (ch - 'a' + 10);
                            } else if (ch >= 'A' && ch <= 'F') {
                                val = (val << 4) | (ch - 'A' + 10);
                            } else {
                                return;
                            }
                        }

                        data.push_back(val);
                    }

                    gdbWriteMemory(addr, data);
                    gdbSendResponse("OK");
                }
            }

        } else if (cmd.substr(0, 3) == "Z0," || cmd.substr(0, 3) == "Z1,") {
            // Add breakpoint
            uint32_t addr = strtoul(cmd.c_str() + 3, nullptr, 16);
            // dprintf("Add breakpoint %08X\n", addr);

            enableBreakpoints = true;
            for (auto &bp : breakpoints) {
                if (bp.addr == addr) {
                    bp.enabled = true;
                    gdbSendResponse("OK");
                    return;
                }
            }

            Breakpoint bp;
            bp.addr    = addr;
            bp.enabled = true;
            breakpoints.push_back(bp);
            gdbSendResponse("OK");

        } else if (cmd.substr(0, 3) == "z0," || cmd.substr(0, 3) == "z1,") {
            // Remove breakpoint
            uint32_t addr = strtoul(cmd.c_str() + 3, nullptr, 16);
            // dprintf("Remove breakpoint %08X\n", addr);

            auto it = breakpoints.begin();
            while (it != breakpoints.end()) {
                if (it->addr == addr) {
                    it = breakpoints.erase(it);
                } else {
                    ++it;
                }
            }

            gdbSendResponse("OK");

        } else if (cmd == "c") {
            // dprintf("Continue\n");
            emuMode = Em_Running;

        } else if (cmd == "s") {
            // Step
            // dprintf("Step\n");
            cpu.emulate();
            gdbSendStopReply();

        } else {
            // Unsupported command
            gdbSendResponse("");
        }
    }

    void gdbSendResponse(const std::string &resp) {
        uint8_t checksum = 0;
        for (uint8_t ch : resp) {
            checksum += ch;
        }

        char csStr[3];
        snprintf(csStr, sizeof(csStr), "%02X", checksum);

        std::string str = "$" + resp + "#" + csStr;
        send(conn, str.c_str(), str.length(), 0);

        // dprintf("< %s\n", str.c_str());
    }

    void gdbSendStopReply(uint8_t signal = 5) {
        std::string resp = fmtstr("S%02X", signal);
        gdbSendResponse(resp);
    }
#endif
};

std::shared_ptr<EmuState> newAq32EmuState() {
    return std::make_shared<Aq32EmuState>();
}
