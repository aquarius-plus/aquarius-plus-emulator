#include "FPGA.h"
#include "Keyboard.h"
#include "EmuState.h"

enum {
    STATUS_KEYB = (1 << 0),
};

class FPGAInt : public FPGA {
public:
    SemaphoreHandle_t mutex;

    FPGAInt() {
    }

    void init() override {
        // Create semaphore for allowing to call FPGA function from different threads
        mutex = xSemaphoreCreateRecursiveMutex();
    }

    bool loadBitstream(const void *data, size_t length) override {
        RecursiveMutexLock lock(mutex);
        ESP_LOGI(TAG, "Starting configuration");

#ifdef EMULATOR
        EmuState::loadCore((const char *)data);
        if (EmuState::get())
            return true;
#endif
        return false;
    }

    void spiSel(bool enable) override {
        auto emuState = EmuState::get();
        if (emuState) {
            emuState->spiSel(enable);
        }
    }

    void spiTx(const void *data, size_t length) override {
        auto emuState = EmuState::get();
        if (emuState) {
            emuState->spiTx(data, length);
        }
    }

    void spiRx(void *buf, size_t length) override {
        auto emuState = EmuState::get();
        if (emuState) {
            emuState->spiRx(buf, length);
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
        RecursiveMutexLock lock(mutex);
        spiSel(true);
        uint8_t cmd[] = {CMD_OVL_TEXT};
        spiTx(cmd, sizeof(cmd));
        spiTx(buf, 2 * 1024);
        spiSel(false);
    }

    void setOverlayFont(const uint8_t buf[2048]) override {
        RecursiveMutexLock lock(mutex);
        spiSel(true);
        uint8_t cmd[] = {CMD_OVL_FONT};
        spiTx(cmd, sizeof(cmd));
        spiTx(buf, 2048);
        spiSel(false);
    }

    void setOverlayPalette(const uint16_t buf[16]) override {
        RecursiveMutexLock lock(mutex);
        spiSel(true);
        uint8_t cmd[] = {CMD_OVL_PALETTE};
        spiTx(cmd, sizeof(cmd));
        spiTx(buf, 2 * 16);
        spiSel(false);
    }

    SemaphoreHandle_t getMutex() override {
        return mutex;
    }
};

FPGA *FPGA::instance() {
    static FPGAInt obj;
    return &obj;
}
