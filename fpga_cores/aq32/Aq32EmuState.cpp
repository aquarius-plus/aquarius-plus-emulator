#include "EmuState.h"
#include "DCBlock.h"
#include "UartProtocol.h"
#include "FPGA.h"
#include "cpu/riscv.h"
#include "Config.h"
#include "bootrom.h"
#include "Aq32Video.h"

#define HCYCLES_PER_LINE   (455)
#define HCYCLES_PER_SAMPLE (162)

#define MAINRAM_START     0x80000000
#define MAINRAM_SIZE_MASK 0x7FFFF
#define TEXTRAM_START     0xFF000000
#define TEXTRAM_SIZE_MASK 0xFFF
#define CHRAM_START       0xFF100000
#define CHRAM_SIZE_MASK   0x7FF
#define VRAM_START        0xFF200000
#define VRAM_SIZE_MASK    0x3FFF
#define SPRATTR_START     0xFF300000
#define SPRATTR_SIZE_MASK 0
#define PALETTE_START     0xFF400000
#define PALETTE_SIZE_MASK 0x7F
#define REG_ESPCTRL       0xFF500000
#define REG_ESPDATA       0xFF500004
#define REG_VCTRL         0xFF500008
#define REG_VSCRX         0xFF50000C
#define REG_VSCRY         0xFF500010
#define REG_VLINE         0xFF500014
#define REG_VIRQLINE      0xFF500018
#define BOOTROM_START     0xFFFFF800

class Aq32EmuState : public EmuState {
public:
    riscv     cpu;
    Aq32Video video;

    int      lineHalfCycles   = 0; // Half-cycles for this line
    int      sampleHalfCycles = 0; // Half-cycles for this sample
    unsigned audioLeft        = 0;
    unsigned audioRight       = 0;
    DCBlock  dcBlockLeft;
    DCBlock  dcBlockRight;

    uint32_t bootRom[0x800 / 4];
    uint32_t mainRam[512 * 1024 / 4]; // Main RAM

    uint8_t keybMatrix[8] = {0}; // Keyboard matrix (8 x 6bits)

    Aq32EmuState() {
        coreType         = 1;
        coreFlags        = 0x02;
        coreVersionMajor = 0;
        coreVersionMinor = 1;
        memcpy(coreName, "Aquarius32      ", sizeof(coreName));

        cpu.dataWrite = [this](uint32_t vaddr, uint32_t val, uint32_t mask) { memWrite(vaddr, val, mask); };
        cpu.dataRead  = [this](uint32_t vaddr) { return memRead(vaddr); };
        cpu.instrRead = [this](uint32_t vaddr) { return memRead(vaddr); };

        memset(keybMatrix, 0xFF, sizeof(keybMatrix));

        memcpy(bootRom, bootrom_bin, bootrom_bin_len);

        loadConfig();

        reset();
    }

    virtual ~Aq32EmuState() {
        saveConfig();
    }

    void loadConfig() {
        auto root = Config::instance()->loadConfigFile("aq32.json");
        // showMemEdit      = getBoolValue(root, "showMemEdit", false);
        // memEditMemSelect = getIntValue(root, "memEditMemSelect", 0);
        // showIoRegsWindow = getBoolValue(root, "showIoRegsWindow", false);

        cJSON_Delete(root);
    }

    void saveConfig() {
        auto root = cJSON_CreateObject();
        // cJSON_AddBoolToObject(root, "showMemEdit", showMemEdit);
        // cJSON_AddNumberToObject(root, "memEditMemSelect", memEditMemSelect);
        // cJSON_AddBoolToObject(root, "showIoRegsWindow", showIoRegsWindow);

        Config::instance()->saveConfigFile("aq32.json", root);
    }

    virtual void reset(bool cold = false) override {
        cpu.regs[0]      = 0;
        cpu.pc           = 0xFFFFF800;
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
    }

    void memWrite(uint32_t addr, uint32_t val, uint32_t mask) {
        if (addr >= MAINRAM_START && addr <= (MAINRAM_START + MAINRAM_SIZE_MASK)) {
            auto p = &mainRam[(addr & MAINRAM_SIZE_MASK) / 4];
            *p     = (*p & ~mask) | (val & mask);
        } else if (addr >= TEXTRAM_START && addr <= (TEXTRAM_START + TEXTRAM_SIZE_MASK)) {
            // Text RAM (8b/16b)
            uint16_t msk = (mask >> 16) | (mask & 0xFFFF);
            auto     p   = &video.textRam[(addr & TEXTRAM_SIZE_MASK) / 2];
            *p           = (*p & ~msk) | (val & msk);
        } else if (addr >= CHRAM_START && addr <= (CHRAM_START + CHRAM_SIZE_MASK)) {
            // Character RAM (8b)
            video.charRam[addr & CHRAM_SIZE_MASK] = val & 0xFF;
        } else if (addr >= VRAM_START && addr <= (VRAM_START + VRAM_SIZE_MASK)) {
            // Video RAM (8/16/32b)
            auto p = reinterpret_cast<uint32_t *>(&video.videoRam[addr & VRAM_SIZE_MASK]);
            *p     = (*p & ~mask) | (val & mask);
        } else if (addr >= SPRATTR_START && addr <= (SPRATTR_START + SPRATTR_SIZE_MASK)) {
            // Sprite attributes
        } else if (addr >= PALETTE_START && addr <= (PALETTE_START + PALETTE_SIZE_MASK)) {
            // Palette (16b)
            video.videoPalette[(addr >> 1) & 63] = val & 0xFFF;
        } else if (addr == REG_ESPCTRL) {
            UartProtocol::instance()->writeCtrl(val & 0xFF);
        } else if (addr == REG_ESPDATA) {
            if (val & 0x100) {
                UartProtocol::instance()->writeCtrl(0x80);
            } else {
                UartProtocol::instance()->writeData(val & 0xFF);
            }
        } else if (addr == REG_VCTRL) {
            video.videoCtrl = val & 0xFF;
            printf("VCTRL=%x\n", val);
        } else if (addr == REG_VSCRX) {
            video.videoScrX = val & 0x1FF;
        } else if (addr == REG_VSCRY) {
            video.videoScrY = val & 0xFF;
        } else if (addr == REG_VLINE) {
        } else if (addr == REG_VIRQLINE) {
        }
    }

    uint32_t memRead(uint32_t addr) {
        if (addr >= MAINRAM_START && addr <= (MAINRAM_START + MAINRAM_SIZE_MASK)) {
            return mainRam[(addr & MAINRAM_SIZE_MASK) / 4];
        } else if (addr >= TEXTRAM_START && addr <= (TEXTRAM_START + TEXTRAM_SIZE_MASK)) {
            // Text RAM (8b/16b)
            uint32_t val = video.textRam[(addr & TEXTRAM_SIZE_MASK) / 2];
            val |= val << 16;
            return val;
        } else if (addr >= CHRAM_START && addr <= (CHRAM_START + CHRAM_SIZE_MASK)) {
            // Character RAM (8b)
            return video.charRam[addr & CHRAM_SIZE_MASK];
        } else if (addr >= VRAM_START && addr <= (VRAM_START + VRAM_SIZE_MASK)) {
            // Video RAM (8/16/32b)
            return reinterpret_cast<uint32_t *>(video.videoRam)[addr & VRAM_SIZE_MASK];
        } else if (addr >= SPRATTR_START && addr <= (SPRATTR_START + SPRATTR_SIZE_MASK)) {
            // Sprite attributes
        } else if (addr >= PALETTE_START && addr <= (PALETTE_START + PALETTE_SIZE_MASK)) {
            // Palette (16b)
            return video.videoPalette[(addr >> 1) & 63];
        } else if (addr == REG_ESPCTRL) {
            return UartProtocol::instance()->readCtrl();
        } else if (addr == REG_ESPDATA) {
            return UartProtocol::instance()->readData();
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
        } else if (addr >= BOOTROM_START) {
            return bootRom[(addr & 0x7FF) / 4];
        }
        return 0;
    }

    virtual bool emulate(int16_t *audioBuf, unsigned numSamples) override {
        // Emulate for 1/60s
        for (int i = 0; i < 8000000 / 60; i++) {
            cpu.emulate(1);
        }
        for (int i = 0; i < 240; i++)
            video.drawLine(i);
        return true;
    }

    virtual void getVideoSize(int &w, int &h) override {
        w = 640;
        h = 480;
    }

    virtual void getPixels(void *pixels, int pitch) override {
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

    virtual void spiTx(const void *data, size_t length) override {
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
        }
    }

    virtual void dbgMenu() override {
        if (!enableDebugger)
            return;

        // ImGui::MenuItem("Memory editor", "", &showMemEdit);
        // ImGui::MenuItem("IO Registers", "", &showIoRegsWindow);
    }

    virtual void dbgWindows() override {
        if (!enableDebugger)
            return;

        // if (showMemEdit)
        //     dbgWndMemEdit(&showMemEdit);
        // if (showIoRegsWindow)
        //     dbgWndIoRegs(&showIoRegsWindow);
    }
};

std::shared_ptr<EmuState> newAq32EmuState() {
    return std::make_shared<Aq32EmuState>();
}
