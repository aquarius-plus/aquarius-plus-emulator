#pragma once

#include "Common.h"

class VDPInterruptDelegate {
public:
    virtual void VDPInterrupt(bool fromLineRender) = 0;
};

class VDP {
public:
    VDP(VDPInterruptDelegate &interruptDelegate);

    bool renderLine();

    int numLines();

    // Registers
    uint8_t regControlRead();
    void    regControlWrite(uint8_t data);

    uint8_t regDataRead();
    void    regDataWrite(uint8_t data);

    uint8_t regVCounterRead();
    uint8_t regHCounterRead();

    const uint32_t *getFramebuffer() { return framebuffer; }

    // private:
    void renderBackground(uint32_t *lineBuf, uint8_t *pixelInFrontOfSpriteBuf);
    void renderSprites(uint32_t *lineBuf, const uint8_t *pixelInFrontOfSpriteBuf);

    bool isIrqPending();

    VDPInterruptDelegate &interruptDelegate;

    uint32_t framebuffer[256 * 192];

    uint8_t  vram[0x4000];
    uint32_t cram[32] = {0};

    uint16_t line        = 0;
    uint8_t  lineCounter = 0xFF;

    uint8_t vdpReg[11] = {0x06, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0xF0, 0x00, 0x00, 0xFF};
    uint8_t vdpReg9latched;

    uint8_t vdpStatus = 0x1F;

    bool bf_toggle = false;

    uint8_t  vdpCodeReg = 0;
    uint16_t vdpAddr    = 0;

    uint8_t rd_data_port_buffer = 0;
};
