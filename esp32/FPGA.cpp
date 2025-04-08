#include "FPGA.h"
#include "Keyboard.h"
#include "EmuState.h"

enum {
    STATUS_KEYB = (1 << 0),
};

class FPGAInt : public FPGA {
public:
    FPGAInt() {
    }

    void init() override {
    }

    bool loadBitstream(const void *data, size_t length) override {
        return true;
    }

    void spiSel(bool enable) override {
        auto es = _EmuState::get();
        if (es) {
            es->spiSel(enable);
        }
    }

    void spiTx(const void *data, size_t length) override {
        auto es = _EmuState::get();
        if (es) {
            es->spiTx(data, length);
        }
    }

    void spiRx(void *buf, size_t length) override {
        auto es = _EmuState::get();
        if (es) {
            es->spiRx(buf, length);
        }
    }

    bool getCoreInfo(CoreInfo *info) override {
        info->coreType     = 1;
        info->flags        = 0x1E;
        info->versionMajor = 1;
        info->versionMinor = 0;
        snprintf(info->name, sizeof(info->name), "Aquarius+");
        return true;
    }

    void setOverlayText(const uint16_t buf[1024]) override {
        // RecursiveMutexLock lock(mutex);
        spiSel(true);
        uint8_t cmd[] = {CMD_OVL_TEXT};
        spiTx(cmd, sizeof(cmd));
        spiTx(buf, 2 * 1024);
        spiSel(false);
    }

    void setOverlayFont(const uint8_t buf[2048]) override {
        // RecursiveMutexLock lock(mutex);
        spiSel(true);
        uint8_t cmd[] = {CMD_OVL_FONT};
        spiTx(cmd, sizeof(cmd));
        spiTx(buf, 2048);
        spiSel(false);
    }

    void setOverlayPalette(const uint16_t buf[16]) override {
        // RecursiveMutexLock lock(mutex);
        spiSel(true);
        uint8_t cmd[] = {CMD_OVL_PALETTE};
        spiTx(cmd, sizeof(cmd));
        spiTx(buf, 2 * 16);
        spiSel(false);
    }

    SemaphoreHandle_t getMutex() override {
        return nullptr;
    }
};

FPGA *FPGA::instance() {
    static FPGAInt obj;
    return &obj;
}
