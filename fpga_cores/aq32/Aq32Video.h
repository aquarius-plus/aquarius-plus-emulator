#pragma once

#include "Common.h"

enum {
    VCTRL_TEXT_ENABLE       = (1 << 0),
    VCTRL_MODE_OFF          = (0 << 1),
    VCTRL_MODE_TILEMAP      = (1 << 1),
    VCTRL_MODE_BITMAP       = (2 << 1),
    VCTRL_MODE_BITMAP_4BPP  = (3 << 1),
    VCTRL_MODE_MASK         = (3 << 1),
    VCTRL_SPRITES_ENABLE    = (1 << 3),
    VCTRL_TEXT_PRIORITY     = (1 << 4),
    VCTRL_REMAP_BORDER_CHAR = (1 << 5),
    VCTRL_80_COLUMNS        = (1 << 6),
    VCTRL_TRAM_PAGE         = (1 << 7),
};

class Aq32Video {
public:
    Aq32Video();
    const uint16_t *getFb() { return screen; }

    static const int activeWidth  = 704;
    static const int activeHeight = 240;

    bool isOnVideoIrqLine() { return videoLine == videoIrqLine; }
    bool isOnStartOfVBlank() { return videoLine == 240; }

    void reset();
    void drawLine(int line);
    int  getLine() { return videoLine; }

    bool nextLine() {
        videoLine++;
        if (videoLine == 262) {
            videoLine = 0;
            return true;
        }
        return false;
    }

    void dbgDrawIoRegs();
    void dbgDrawSpriteRegs();
    void dbgDrawPaletteRegs();

    alignas(4) uint16_t textRam[2048];      // Screen RAM for text mode
    alignas(4) uint8_t videoRam[16 * 1024]; // Video RAM
    alignas(4) uint8_t charRam[2048];       // Character RAM

    uint8_t  videoCtrl        = 0;   // $E0   : Video control register
    uint16_t videoScrX        = 0;   // $E1/E2: Tile map horizontal scroll register
    uint8_t  videoScrY        = 0;   // $E3   : Tile map horizontal scroll register
    uint8_t  videoSprSel      = 0;   // $E4   : Sprite select
    uint16_t videoSprX[64]    = {0}; // $E5/E6: Sprite X-position
    uint8_t  videoSprY[64]    = {0}; // $E7   : Sprite Y-position
    uint16_t videoSprIdx[64]  = {0}; // $E8/E9: Sprite tile index
    uint8_t  videoSprAttr[64] = {0}; // $E9   : Sprite attributes
    uint8_t  videoPalSel      = 0;   // $EA   : Palette entry select
    uint16_t videoPalette[64] = {0}; // $EB   : Video palette
    uint16_t videoLine        = 0;   // $EC   : Current line number
    uint8_t  videoIrqLine     = 0;   // $ED   : Line number at which to generate IRQ

    uint16_t screen[activeWidth * activeHeight];
};
