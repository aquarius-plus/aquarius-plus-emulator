#pragma once

#include "EmuState.h"
#include "AqpVideo.h"
#include "z80.h"

class AqpEmuState : public _EmuState {
public:
    AqpEmuState();

    void spiSel(bool enable) override;
    void spiTx(const void *data, size_t length) override;
    void spiRx(void *buf, size_t length) override;

    Z80Context z80ctx;                  // Z80 emulation core state
    AqpVideo   video;                   // Video
    int        lineHalfCycles    = 0;   // Half-cycles for this line
    int        sampleHalfCycles  = 0;   // Half-cycles for this sample
    uint8_t    keybMatrix[8]     = {0}; // Keyboard matrix (8 x 6bits)
    bool       cartridgeInserted = false;

    std::vector<uint8_t> txBuf;
    bool                 enabled = false;
};
