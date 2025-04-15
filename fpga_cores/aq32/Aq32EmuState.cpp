#include "EmuState.h"
#include "DCBlock.h"
#include "UartProtocol.h"
#include "FPGA.h"
#include "cpu/riscv.h"
#include "Config.h"

#define HCYCLES_PER_LINE   (455)
#define HCYCLES_PER_SAMPLE (162)

class Aq32EmuState : public EmuState {
public:
    riscv cpu;

    int      lineHalfCycles   = 0; // Half-cycles for this line
    int      sampleHalfCycles = 0; // Half-cycles for this sample
    unsigned audioLeft        = 0;
    unsigned audioRight       = 0;
    DCBlock  dcBlockLeft;
    DCBlock  dcBlockRight;

    uint8_t mainRam[512 * 1024]; // Main RAM

    uint8_t keybMatrix[8] = {0}; // Keyboard matrix (8 x 6bits)

    Aq32EmuState() {
        coreType         = 1;
        coreFlags        = 0x02;
        coreVersionMajor = 0;
        coreVersionMinor = 1;
        memcpy(coreName, "Aquarius32      ", sizeof(coreName));

        cpu.dataWrite = [this](uint32_t vaddr, uint32_t val, uint32_t mask) { dataWrite(vaddr, val, mask); };
        cpu.dataRead  = [this](uint32_t vaddr) { return dataRead(vaddr); };
        cpu.dataRead  = [this](uint32_t vaddr) { return dataRead(vaddr); };

        memset(keybMatrix, 0xFF, sizeof(keybMatrix));

        loadConfig();
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
    }

    void dataWrite(uint32_t vaddr, uint32_t val, uint32_t mask) {
    }

    uint32_t dataRead(uint32_t vaddr) {
        return 0;
    }

    uint32_t instrRead(uint32_t vaddr) {
        return 0;
    }

    virtual bool emulate(int16_t *audioBuf, unsigned numSamples) override {
        return true;
    }

    virtual void getVideoSize(int &w, int &h) override {
        w = 640;
        h = 480;
    }

    virtual void getPixels(void *pixels, int pitch) override {
        for (int j = 0; j < 480; j++) {
            for (int i = 0; i < 640; i++) {
                ((uint32_t *)((uintptr_t)pixels + j * pitch))[i] = 0;
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
