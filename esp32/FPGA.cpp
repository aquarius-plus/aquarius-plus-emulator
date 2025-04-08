#include "FPGA.h"
#include "Keyboard.h"
#include "EmuState.h"

enum {
    // Aq+ command
    CMD_RESET           = 0x01,
    CMD_SET_KEYB_MATRIX = 0x10,
    CMD_SET_HCTRL       = 0x11,
    CMD_WRITE_KBBUF     = 0x12,
    CMD_BUS_ACQUIRE     = 0x20,
    CMD_BUS_RELEASE     = 0x21,
    CMD_MEM_WRITE       = 0x22,
    CMD_MEM_READ        = 0x23,
    CMD_IO_WRITE        = 0x24,
    CMD_IO_READ         = 0x25,
    CMD_ROM_WRITE       = 0x30,
    CMD_SET_VIDMODE     = 0x40,

    // General commands
    CMD_GET_KEYS    = 0xF1, // MorphBook specific
    CMD_SET_VOLUME  = 0xF3, // MorphBook specific
    CMD_OVL_TEXT    = 0xF4,
    CMD_OVL_FONT    = 0xF5,
    CMD_OVL_PALETTE = 0xF6,
    CMD_GET_STATUS  = 0xF7,
    CMD_GET_SYSINFO = 0xF8,
    CMD_GET_NAME1   = 0xF9,
    CMD_GET_NAME2   = 0xFA,
};

enum {
    STATUS_KEYB = (1 << 0),
};

class FPGAInt : public FPGA {
public:
    std::vector<uint8_t> txBuf;
    bool                 enabled = false;

    FPGAInt() {
    }

    void init() override {
    }

    bool loadBitstream(const void *data, size_t length) override {
        return true;
    }

    void spiSel(bool enable) override {
        if (enabled == enable)
            return;
        enabled = enable;

        if (!enabled && !txBuf.empty()) {
            switch (txBuf[0]) {
                case CMD_RESET: {
                    if (txBuf.size() >= 1 + 1) {
                        if (txBuf[1] & 2) {
                            emuState.coldReset();
                        } else {
                            emuState.warmReset();
                        }
                    }
                    break;
                }

                case CMD_SET_KEYB_MATRIX: {
                    if (txBuf.size() >= 1 + 8) {
                        memcpy(&emuState.keybMatrix, &txBuf[1], 8);
                    }
                    break;
                }

                case CMD_SET_HCTRL: {
                    if (txBuf.size() >= 1 + 2) {
                        emuState.ay1.portRdData[0] = txBuf[1];
                        emuState.ay1.portRdData[1] = txBuf[2];
                    }
                    break;
                }

                case CMD_WRITE_KBBUF: {
                    if (txBuf.size() >= 1 + 1) {
                        emuState.kbBufWrite(txBuf[1]);
                    }
                    break;
                }
            }
        }

        txBuf.clear();
    }

    void spiTx(const void *data, size_t length) override {
        if (!enabled)
            return;
        auto p = static_cast<const uint8_t *>(data);
        txBuf.insert(txBuf.end(), p, p + length);
    }

    void spiRx(void *buf, size_t length) override {
        if (!enabled)
            return;
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
