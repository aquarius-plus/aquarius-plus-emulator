#include "VDP.h"

// TODO:
// - Sprite zooming
// - Japanese Y's bug (something with nametable mirroring?)
// - Maybe generate sprite line one line before to correctly(?) generate overflow bit
//

#define VDP_DEBUG 0

int VDP::numLines() {
    return 262;
}

// vdpReg0:
// 7 VScroll inhibit
// 6 HScroll inhibit
// 5 Left col blank
// 4 IE1 - H-Line interrupt
// 3 Sprite shift bit
// 2 1
// 1 1
// 0 0

// vdpReg1:
// 7 1
// 6 Screen enable
// 5 IE - V-blank interrupt
// 4 0
// 3 0
// 2 0
// 1 16-pix sprite height
// 0 sprite mag bit

// vdpReg2: base address of screen map
// vdpReg3/4: always 0xFF
// vdpReg5: base address of sprite attribute table
// vdpReg6: base address of sprite patterns
// vdpReg7: border color index
// vdpReg8: horizontal scroll value
// vdpReg9: vertical scroll value
// vdpReg10: raster line interrupt

VDP::VDP(VDPInterruptDelegate &interruptDelegate)
    : interruptDelegate(interruptDelegate) {
}

uint8_t VDP::regControlRead() {
    uint8_t result = vdpStatus;
    vdpStatus &= 0x1f;
    bf_toggle = false;
    return result;
}

void VDP::regControlWrite(uint8_t data) {
    if (!bf_toggle) {
        vdpAddr = (vdpAddr & 0x3F00) | data;
    } else {
        vdpCodeReg = data >> 6;
        vdpAddr    = ((data & 0x3F) << 8) | (vdpAddr & 0xFF);

        if (vdpCodeReg == 0) {
            // Read VDP ram
            rd_data_port_buffer = vram[vdpAddr];
            vdpAddr             = (vdpAddr + 1) & 0x3FFF;

        } else if (vdpCodeReg == 2) {
            // Write VDP register
            if ((data & 0xF) < 11) {
                vdpReg[data & 0xF] = vdpAddr & 0xFF;
            }
        }
    }
    bf_toggle = !bf_toggle;
}

uint8_t VDP::regDataRead() {
    bf_toggle = false;

    uint8_t result      = rd_data_port_buffer;
    rd_data_port_buffer = vram[vdpAddr];
    vdpAddr             = (vdpAddr + 1) & 0x3FFF;
    return result;
}

void VDP::regDataWrite(uint8_t data) {
    bf_toggle = false;

    rd_data_port_buffer = data;
    if (vdpCodeReg == 3) {
        uint8_t r = (data >> 0) & 3;
        uint8_t g = (data >> 2) & 3;
        uint8_t b = (data >> 4) & 3;

        r = (r << 6) | (r << 4) | (r << 2) | r;
        g = (g << 6) | (g << 4) | (g << 2) | g;
        b = (b << 6) | (b << 4) | (b << 2) | b;

        cram[vdpAddr & 31] = (r << 0) | (g << 8) | (b << 16);

    } else {
        vram[vdpAddr] = data;
        //        cout << "vram[" << toHex(vdpAddr) << "] = " << toHex(data) << endl;
    }

    vdpAddr = (vdpAddr + 1) & 0x3FFF;
}

uint8_t VDP::regVCounterRead() {
    // ntsc: 0x00 - 0xDA, 0xD5 - 0xFF
    return (line <= 0xDA) ? line : (line - (0xDA - 0xD5 + 1));
}

uint8_t VDP::regHCounterRead() {
    // H-counter port, not implemented
    return 0xFF;
}

bool VDP::isIrqPending() {
    return (
        (vdpStatus & 0x80) != 0 && (vdpReg[1] & 0x20) != 0);
}

void VDP::renderBackground(uint32_t *lineBuf, uint8_t *pixelInFrontOfSpriteBuf) {
    uint8_t offsetX = vdpReg[8];             // x-scroll
    int     offsetY = line + vdpReg9latched; // y-scroll

    bool hscrollInhibit = (vdpReg[0] & 0x40) != 0;
    if (hscrollInhibit && line < 16) {
        offsetX = 0;
    }
    bool vscrollInhibit = (vdpReg[0] & 0x80) != 0;

    uint16_t mapBase = (vdpReg[2] & 0x0E) << 9;
    uint16_t addr    = mapBase + 32 * ((offsetY / 8) % 28);

    bool leftColumnBlank = (vdpReg[0] & 0x20) != 0;

    // Render tilemap
    for (int i = 0; i < 32; i++) {
        uint16_t entryAddr = (addr + i) * 2;
        uint16_t entry     = vram[entryAddr + 0] | (vram[entryAddr + 1] << 8);

        uint16_t tileDataAddr         = (entry & 0x1FF) * 32;
        bool     flipHorizontal       = (entry & 0x200) != 0;
        bool     flipVertical         = (entry & 0x400) != 0;
        bool     useSpritePalette     = (entry & 0x800) != 0;
        bool     tileInFrontOfSprites = (entry & 0x1000) != 0;

        int y = flipVertical ? 7 - (offsetY & 7) : (offsetY & 7);

        uint32_t tileData =
            (vram[tileDataAddr + y * 4 + 0] << 0) |
            (vram[tileDataAddr + y * 4 + 1] << 8) |
            (vram[tileDataAddr + y * 4 + 2] << 16) |
            (vram[tileDataAddr + y * 4 + 3] << 24);

        for (int k = 0; k < 8; k++) {
            uint8_t colorIdx = 0;
            if (!flipHorizontal) {
                colorIdx =
                    ((tileData & (1 << ((7 - k) + 0))) ? 1 : 0) |
                    ((tileData & (1 << ((7 - k) + 8))) ? 2 : 0) |
                    ((tileData & (1 << ((7 - k) + 16))) ? 4 : 0) |
                    ((tileData & (1 << ((7 - k) + 24))) ? 8 : 0);
            } else {
                colorIdx =
                    ((tileData & (1 << (k + 0))) ? 1 : 0) |
                    ((tileData & (1 << (k + 8))) ? 2 : 0) |
                    ((tileData & (1 << (k + 16))) ? 4 : 0) |
                    ((tileData & (1 << (k + 24))) ? 8 : 0);
            }
            uint32_t color = cram[useSpritePalette ? (16 + colorIdx) : colorIdx];

            // background color index 0 is always behind sprites
            pixelInFrontOfSpriteBuf[offsetX] = colorIdx == 0 ? 0 : tileInFrontOfSprites;

            if (leftColumnBlank && offsetX < 8) {
                color                            = cram[16 + (vdpReg[7] & 0xF)];
                pixelInFrontOfSpriteBuf[offsetX] = true;
            }

            lineBuf[offsetX++] = color;
        }
    }
}

void VDP::renderSprites(uint32_t *lineBuf, const uint8_t *pixelInFrontOfSpriteBuf) {
    uint8_t spritePixelDrawn[256];
    memset(spritePixelDrawn, 0, sizeof(spritePixelDrawn));

    bool leftColumnBlank = (vdpReg[0] & 0x20) != 0;

    int line2 = line - 1;

    bool     doubleHeight        = (vdpReg[1] & 0x02) != 0;
    uint16_t spritePatternBase   = ((vdpReg[6] & 0x04) == 0) ? 0x0000 : 0x2000;
    uint16_t spriteAttributeBase = (vdpReg[5] & 0x7E) << 7;

    uint8_t spritesDrawnCount = 0;
    uint8_t spritesToDraw[8];

    for (int i = 0; i < 64; i++) {
        int vpos = vram[spriteAttributeBase + i];
        if (vpos == 0xD0) {
            // terminator
            break;
        }
        if (vpos >= 240) {
            vpos -= 256;
        }

        int vpos2 = vpos + (doubleHeight ? 16 : 8) - 1;
        if (line2 < vpos || line2 > vpos2) {
            continue;
        }

        if (spritesDrawnCount >= 8) {
            vdpStatus |= 0x40;
            break;
        }
        spritesToDraw[spritesDrawnCount++] = i;
    }

    for (int i = spritesDrawnCount - 1; i >= 0; i--) {
        int spriteIdx = spritesToDraw[i];

        int vpos = vram[spriteAttributeBase + spriteIdx];
        if (vpos >= 240) {
            vpos -= 256;
        }

        uint8_t hpos     = vram[spriteAttributeBase + 0x80 + spriteIdx * 2 + 0];
        uint8_t charcode = vram[spriteAttributeBase + 0x80 + spriteIdx * 2 + 1];

        if (doubleHeight) {
            charcode &= ~1;
        }

        uint8_t charline = line2 - vpos;
        if (charline >= 8) {
            charcode |= 1;
        }
        charline &= 7;

        uint16_t tileDataAddr = spritePatternBase | (charcode * 32);

        uint32_t tileData =
            (vram[tileDataAddr + charline * 4 + 0] << 0) |
            (vram[tileDataAddr + charline * 4 + 1] << 8) |
            (vram[tileDataAddr + charline * 4 + 2] << 16) |
            (vram[tileDataAddr + charline * 4 + 3] << 24);

        int hpos2 = (int)hpos + 8;
        if (hpos2 > 256) {
            hpos2 = 256;
        }

        for (int x = hpos, k = 0; x < hpos2; x++, k++) {
            uint8_t colorIdx =
                ((tileData & (1 << ((7 - k) + 0))) ? 1 : 0) |
                ((tileData & (1 << ((7 - k) + 8))) ? 2 : 0) |
                ((tileData & (1 << ((7 - k) + 16))) ? 4 : 0) |
                ((tileData & (1 << ((7 - k) + 24))) ? 8 : 0);

            if (colorIdx == 0) {
                continue;
            }

            uint32_t color = cram[16 + colorIdx];

            if ((leftColumnBlank && x < 8) || pixelInFrontOfSpriteBuf[x]) {
                continue;
            }

            if (spritePixelDrawn[x]) {
                // two sprites touched
                vdpStatus |= 0x20;
            }

            lineBuf[x]          = color;
            spritePixelDrawn[x] = 1;
        }

        if (vdpReg[0] & 0x08) {
            printf("Sprite shift\n");
        }
    }
}

bool VDP::renderLine() {
    if (line == 0) {
        vdpReg9latched = vdpReg[9];
    }

    if (line < 192) {
        uint32_t *fbp       = &framebuffer[line * 256];
        bool      displayOn = (vdpReg[1] & 0x40) != 0;

        if (!displayOn) {
            memset(fbp, 0, 256 * sizeof(uint32_t));
        } else {
            uint8_t pixelInFrontOfSprites[256];

            renderBackground(fbp, pixelInFrontOfSprites);
            renderSprites(fbp, pixelInFrontOfSprites);
        }
    }

    if (line == 193) {
        vdpStatus |= 0x80;
    }

    if (line < 193) {
        lineCounter--;
        if (lineCounter == 0xFF) {
            lineCounter = vdpReg[10];

            // Line interrupts are only generated once (they do not pend)
            interruptDelegate.VDPInterrupt(true);
        }
    } else {
        lineCounter = vdpReg[10];
    }

    if (line == 261) {
        line = 0;
        return true;
    }

    line++;
    return false;
}
