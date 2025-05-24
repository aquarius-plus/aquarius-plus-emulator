#pragma once

#include "Common.h"

class Aq32Video {
public:
    enum {
        VCTRL_TEXT_ENABLE         = (1 << 0),
        VCTRL_GFXMODE_OFF         = (0 << 1),
        VCTRL_GFXMODE_TILEMAP     = (1 << 1),
        VCTRL_GFXMODE_BITMAP_4BPP = (3 << 1),
        VCTRL_GFXMODE_MASK        = (3 << 1),
        VCTRL_SPRITES_ENABLE      = (1 << 3),
        VCTRL_TEXT_PRIORITY       = (1 << 4),
        VCTRL_80_COLUMNS          = (1 << 6),
        VCTRL_TRAM_PAGE           = (1 << 7),
    };

    Aq32Video();
    const uint16_t *getFb() { return screen; }

    static const int activeWidth  = 704;
    static const int activeHeight = 240;

    bool isOnVideoIrqLine() { return videoLine == videoIrqLine; }
    bool isOnStartOfVBlank() { return videoLine == 240; }

    void reset();
    void drawLine(int line);

    void dbgDrawIoRegs();
    void dbgDrawSpriteRegs();
    void dbgDrawPaletteRegs();

    alignas(4) uint16_t textRam[2048];      // Screen RAM for text mode
    alignas(4) uint8_t videoRam[32 * 1024]; // Video RAM
    alignas(4) uint8_t charRam[2048];       // Character RAM
    uint16_t videoPalette[64] = {0};        // Video palette

    struct Sprite {
        uint16_t x    = 0;
        uint8_t  y    = 0;
        uint16_t attr = 0;
    };
    Sprite sprites[64];

    uint8_t  videoCtrl    = 0; // Video control register
    uint16_t videoScrX    = 0; // Tile map horizontal scroll register
    uint8_t  videoScrY    = 0; // Tile map horizontal scroll register
    uint16_t videoLine    = 0; // Current line number
    uint8_t  videoIrqLine = 0; // Line number at which to generate IRQ

    uint16_t screen[activeWidth * activeHeight];
};
