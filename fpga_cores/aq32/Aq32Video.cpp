#include "Aq32Video.h"
#include "imgui.h"

Aq32Video::Aq32Video() {
    memset(videoPalette, 0, sizeof(videoPalette));
    memset(textRam, 0, sizeof(textRam));
    memset(videoRam, 0, sizeof(videoRam));
    memset(charRam, 0, sizeof(charRam));
}

void Aq32Video::reset() {
    videoCtrl    = 0;
    videoScrX    = 0;
    videoScrY    = 0;
    videoLine    = 0;
    videoIrqLine = 0;
}

void Aq32Video::drawLine(int line) {
    if (line < 0 || line >= activeHeight)
        return;

    bool vActive = line >= 16 && line < 216;

    // Render text
    uint8_t lineText[1024];
    {
        bool mode80      = (videoCtrl & VCTRL_80_COLUMNS) != 0;
        bool tramPage    = (videoCtrl & VCTRL_TRAM_PAGE) != 0;
        bool remapBorder = (videoCtrl & VCTRL_REMAP_BORDER_CHAR) != 0;

        unsigned idx = 1024 - 32;
        for (int i = 0; i < activeWidth; i++) {
            // Draw text character
            unsigned addr = 0;

            if (vActive && idx < 640) {
                int row = (line - 16) / 8;
                if (mode80) {
                    int column = (i - 32) / 8;
                    addr       = row * 80 + column;
                } else {
                    int column = ((i / 2) - 16) / 8;
                    addr       = row * 40 + column;
                }
            } else {
                if (remapBorder) {
                    addr = mode80 ? 0x7FF : 0x3FF;
                } else {
                    addr = 0;
                }
            }
            if (!mode80) {
                addr = (addr & 0x3FF) | (tramPage ? 0x400 : 0);
            }

            uint16_t val    = textRam[addr];
            uint8_t  ch     = val & 0xFF;
            uint8_t  color  = val >> 8;
            uint8_t  charBm = charRam[ch * 8 + (line & 7)];

            lineText[idx] = (charBm & (1 << (7 - ((mode80 ? i : (i / 2)) & 7)))) ? (color >> 4) : (color & 0xF);
            idx           = (idx + 1) & 1023;
        }
    }

    // Render bitmap/tile layer
    uint8_t lineGfx[512];
    if (vActive) {
        int bmline = line - 16;
        switch (videoCtrl & VCTRL_MODE_MASK) {
            case VCTRL_MODE_BITMAP: {
                // Bitmap mode 1bpp
                for (int i = 0; i < 320; i++) {
                    int     row    = bmline / 8;
                    int     column = i / 8;
                    uint8_t col    = videoRam[0x2000 + row * 40 + column];
                    uint8_t bm     = videoRam[0x0000 + bmline * 40 + column];
                    uint8_t color  = (bm & (1 << (7 - (i & 7)))) ? (col >> 4) : (col & 0xF);

                    lineGfx[i] = (1 << 4) | color;
                }
                break;
            }

            case VCTRL_MODE_BITMAP_4BPP: {
                // Bitmap mode 4bpp
                for (int i = 0; i < 80; i++) {
                    uint8_t col = videoRam[bmline * 80 + i];

                    lineGfx[i * 4 + 0] = (1 << 4) | (col >> 4);
                    lineGfx[i * 4 + 1] = (1 << 4) | (col >> 4);
                    lineGfx[i * 4 + 2] = (1 << 4) | (col & 0xF);
                    lineGfx[i * 4 + 3] = (1 << 4) | (col & 0xF);
                }
                break;
            }

            case VCTRL_MODE_TILEMAP: {
                // Tile mode
                unsigned idx      = (-(videoScrX & 7)) & 511;
                unsigned tileLine = (bmline + videoScrY) & 255;
                unsigned row      = (tileLine >> 3) & 31;
                unsigned col      = videoScrX >> 3;

                for (int i = 0; i < 41; i++) {
                    // Tilemap is 64x32 (2 bytes per entry)

                    // Fetch tilemap entry
                    uint8_t entryL = videoRam[(row << 7) | (col << 1)];
                    uint8_t entryH = videoRam[(row << 7) | (col << 1) | 1];

                    unsigned tileIdx = ((entryH & 1) << 8) | entryL;
                    bool     hFlip   = (entryH & (1 << 1)) != 0;
                    bool     vFlip   = (entryH & (1 << 2)) != 0;
                    uint8_t  attr    = entryH & 0x70;

                    unsigned patOffs = (tileIdx << 5) | ((tileLine & 7) << 2);
                    if (vFlip)
                        patOffs ^= (7 << 2);

                    // Next column
                    col = (col + 1) & 63;

                    for (int n = 0; n < 4; n++) {
                        int m = n;
                        if (hFlip)
                            m ^= 3;

                        uint8_t data = videoRam[patOffs + m];

                        if (!hFlip) {
                            uint8_t val = data >> 4;
                            val |= (attr & ((val == 0) ? 0x30 : 0x70));
                            lineGfx[idx++] = val;
                            idx &= 511;
                        }
                        {
                            uint8_t val = data & 0xF;
                            val |= (attr & ((val == 0) ? 0x30 : 0x70));
                            lineGfx[idx++] = val;
                            idx &= 511;
                        }
                        if (hFlip) {
                            uint8_t val = data >> 4;
                            val |= (attr & ((val == 0) ? 0x30 : 0x70));
                            lineGfx[idx++] = val;
                            idx &= 511;
                        }
                    }
                }
                break;
            }

            default: {
                for (int i = 0; i < 320; i++) {
                    lineGfx[i] = 0;
                }
                break;
            }
        }

        // Render sprites
        if ((videoCtrl & (1 << 3)) != 0) {
            for (int i = 0; i < 64; i++) {
                const auto &sprite = sprites[i];

                // Check if sprite enabled
                bool enabled = (sprite.attr & (1 << 7)) != 0;
                if (!enabled)
                    continue;

                // Check if sprite is visible on this line
                bool h16     = (sprite.attr & (1 << 3)) != 0;
                int  sprLine = (bmline - sprite.y) & 0xFF;
                if (sprLine >= (h16 ? 16 : 8))
                    continue;

                int      sprX     = sprite.x;
                unsigned tileIdx  = sprite.idx;
                bool     hFlip    = (sprite.attr & (1 << 1)) != 0;
                bool     vFlip    = (sprite.attr & (1 << 2)) != 0;
                uint8_t  palette  = sprite.attr & 0x30;
                bool     priority = (sprite.attr & (1 << 6)) != 0;

                unsigned idx = sprX;

                if (vFlip)
                    sprLine ^= (h16 ? 15 : 7);

                tileIdx ^= (sprLine >> 3);

                unsigned patOffs = (tileIdx << 5) | ((sprLine & 7) << 2);

                for (int n = 0; n < 4; n++) {
                    int m = n;
                    if (hFlip)
                        m ^= 3;

                    uint8_t data = videoRam[patOffs + m];

                    if (!hFlip) {
                        if (priority || (lineGfx[idx] & (1 << 6)) == 0) {
                            unsigned colIdx = (data >> 4);

                            if (colIdx != 0)
                                lineGfx[idx] = colIdx | palette;
                        }
                        idx++;
                        idx &= 511;
                    }
                    if (priority || (lineGfx[idx] & (1 << 6)) == 0) {
                        unsigned colIdx = (data & 0xF);

                        if (colIdx != 0)
                            lineGfx[idx] = colIdx | palette;
                    }
                    idx++;
                    idx &= 511;

                    if (hFlip) {
                        if (priority || (lineGfx[idx] & (1 << 6)) == 0) {
                            unsigned colIdx = (data >> 4);

                            if (colIdx != 0)
                                lineGfx[idx] = colIdx | palette;
                        }
                        idx++;
                        idx &= 511;
                    }
                }
            }
        }
    }

    // Compose layers
    {
        uint16_t *pd  = &screen[line * activeWidth];
        unsigned  idx = 1024 - 32;

        for (int i = 0; i < activeWidth; i++) {
            bool active       = idx < 640 && vActive;
            bool textPriority = (videoCtrl & VCTRL_TEXT_PRIORITY) != 0;
            bool textEnable   = (videoCtrl & VCTRL_TEXT_ENABLE) != 0;

            uint8_t colIdx = 0;
            if (!active) {
                if (textEnable)
                    colIdx = lineText[idx];
            } else {
                if (textEnable)
                    colIdx = lineText[idx];

                if (textEnable && !textPriority)
                    colIdx = lineText[idx];
                if (!textEnable || textPriority || (lineGfx[idx / 2] & 0xF) != 0)
                    colIdx = lineGfx[idx / 2];
                if (textEnable && textPriority && (lineText[idx] & 0xF) != 0)
                    colIdx = lineText[idx];
            }

            pd[i] = videoPalette[colIdx & 0x3F];
            idx   = (idx + 1) & 1023;
        }
    }
}

void Aq32Video::dbgDrawIoRegs() {
    static const char *gfxMode[] = {"OFF", "TILEMAP", "BITMAP", "BITMAP_4BPP"};

    ImGui::Text("VCTRL   : 0x%02X", videoCtrl);
    ImGui::Text("  TEXT_ENABLE       : %u", videoCtrl & 1);
    ImGui::Text("  MODE              : %u (%s)", (videoCtrl >> 1) & 3, gfxMode[(videoCtrl >> 1) & 3]);
    ImGui::Text("  SPRITES_ENABLE    : %u", (videoCtrl >> 3) & 1);
    ImGui::Text("  TEXT_PRIORITY     : %u", (videoCtrl >> 4) & 1);
    ImGui::Text("  REMAP_BORDER_CHAR : %u", (videoCtrl >> 5) & 1);
    ImGui::Text("  80_COLUMNS        : %u", (videoCtrl >> 6) & 1);
    ImGui::Text("  TRAM_PAGE         : %u", (videoCtrl >> 7) & 1);
    ImGui::Text("VSCRX   : %u", videoScrX);
    ImGui::Text("VSCRY   : %u", videoScrY);
    ImGui::Text("VLINE   : %u", videoLine);
    ImGui::Text("VIRQLINE: %u", videoIrqLine);
}

void Aq32Video::dbgDrawSpriteRegs() {
    if (ImGui::BeginTable("Table", 10, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Tile", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("En", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Pri", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Pal", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("H16", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("VF", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("HF");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(64);
        while (clipper.Step()) {
            for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++) {
                const auto &sprite = sprites[row_n];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%2d", row_n);
                ImGui::TableNextColumn();
                ImGui::Text("%3d", sprite.x);
                ImGui::TableNextColumn();
                ImGui::Text("%3d", sprite.y);
                ImGui::TableNextColumn();
                ImGui::Text("%3d", sprite.idx);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((sprite.attr & 0x80) ? "X" : "");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((sprite.attr & 0x40) ? "X" : "");
                ImGui::TableNextColumn();
                ImGui::Text("%d", (sprite.attr >> 4) & 3);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((sprite.attr & 0x08) ? "X" : "");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((sprite.attr & 0x04) ? "X" : "");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((sprite.attr & 0x02) ? "X" : "");
            }
        }
        ImGui::EndTable();
    }
}

void Aq32Video::dbgDrawPaletteRegs() {
    if (ImGui::BeginTable("Table", 8, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Pal", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("G", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Color");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(64);
        while (clipper.Step()) {
            for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++) {
                int r = (videoPalette[row_n] >> 8) & 0xF;
                int g = (videoPalette[row_n] >> 4) & 0xF;
                int b = (videoPalette[row_n] >> 0) & 0xF;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%2d", row_n);
                ImGui::TableNextColumn();
                ImGui::Text("%d", row_n / 16);
                ImGui::TableNextColumn();
                ImGui::Text("%2d", row_n & 15);
                ImGui::TableNextColumn();
                ImGui::Text("%03X", videoPalette[row_n]);
                ImGui::TableNextColumn();
                ImGui::Text("%2d", r);
                ImGui::TableNextColumn();
                ImGui::Text("%2d", g);
                ImGui::TableNextColumn();
                ImGui::Text("%2d", b);
                ImGui::TableNextColumn();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32((ImVec4)ImColor((r << 4) | r, (g << 4) | g, (b << 4) | b)));
            }
        }
        ImGui::EndTable();
    }
}
