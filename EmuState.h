#pragma once

#include "Common.h"
#include "z80.h"
#include "AqpVideo.h"
#include "AY8910.h"
#include <deque>
#include "Config.h"

#define ERF_RENDER_SCREEN    (1 << 0)
#define ERF_NEW_AUDIO_SAMPLE (1 << 1)

class EmuState {
public:
    static std::shared_ptr<EmuState> get();
    static void                      loadCore(const std::string &name);

    virtual void loadConfig(cJSON *root) = 0;
    virtual void saveConfig(cJSON *root) = 0;

    virtual void reset(bool cold = false)                        = 0;
    virtual bool emulate(int16_t *audioBuf, unsigned numSamples) = 0;
    virtual void getVideoSize(int &w, int &h)                    = 0;
    virtual void getPixels(void *pixels, int pitch)              = 0;

    virtual void spiSel(bool enable)                    = 0;
    virtual void spiTx(const void *data, size_t length) = 0;
    virtual void spiRx(void *buf, size_t length)        = 0;

    virtual void fileMenu() {}
    virtual void pasteText(const std::string &str) {}
    virtual bool pasteIsDone() { return true; }

    virtual bool getDebuggerEnabled()        = 0;
    virtual void setDebuggerEnabled(bool en) = 0;
    virtual void dbgMenu()                   = 0;
    virtual void dbgWindows()                = 0;
};
