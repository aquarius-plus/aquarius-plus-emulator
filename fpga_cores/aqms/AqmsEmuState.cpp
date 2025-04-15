#include "EmuState.h"

class AqmsEmuState : public EmuState {
public:
    AqmsEmuState() {
        coreType         = 1;
        coreFlags        = 0;
        coreVersionMajor = 1;
        coreVersionMinor = 0;
        memcpy(coreName, "Master System   ", sizeof(coreName));
    }

    virtual void reset(bool cold = false) override {
    }

    virtual bool emulate(int16_t *audioBuf, unsigned numSamples) override {
        return true;
    }

    virtual void getVideoSize(int &w, int &h) override {
        w = 640;
        h = 480;
    }

    virtual void getPixels(void *pixels, int pitch) override {
        renderOverlay(pixels, pitch);
    }

    virtual void spiTx(const void *data, size_t length) override {
        EmuState::spiTx(data, length);
        if (!spiSelected || txBuf.empty())
            return;
    }

    virtual void dbgMenu() override {
    }

    virtual void dbgWindows() override {
    }
};

std::shared_ptr<EmuState> newAqmsEmuState() {
    return std::make_shared<AqmsEmuState>();
}
