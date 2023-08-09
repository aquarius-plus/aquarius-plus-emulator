#include "Common.h"
#include <SDL.h>
#ifndef _WIN32
#    undef main
#endif

#include "EmuState.h"

#include "Video.h"
#include "AqKeyboard.h"
#include "Audio.h"
#include "AqUartProtocol.h"
#include "SDCardVFS.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "tinyfiledialogs.h"

#include "imgui_memory_editor.h"

#if _WIN32
#    include <Windows.h>
#    include <shlobj.h>
#endif

static uint8_t mem_read(size_t param, uint16_t addr) {
    (void)param;

    // Handle CPM remap bit
    if (emuState.cpmRemap) {
        if (addr < 0x4000)
            addr += 0xC000;
        if (addr >= 0xC000)
            addr -= 0xC000;
    }

    // Get and decode banking register
    uint8_t  bankreg     = emuState.bankRegs[addr >> 14];
    unsigned page        = bankreg & 0x3F;
    bool     overlay_ram = (bankreg & (1 << 6)) != 0;

    addr &= 0x3FFF;

    if (overlay_ram && addr >= 0x3000) {
        if (addr < 0x3400) {
            return emuState.screenRam[addr & 0x3FF];
        } else if (addr < 0x3800) {
            return emuState.colorRam[addr & 0x3FF];
        } else {
            return emuState.mainRam[addr];
        }
    }

    if (page < 16) {
        return emuState.flashRom[page * 0x4000 + addr];
    } else if (page == 19) {
        return emuState.cartridgeInserted ? emuState.cartRom[addr] : 0xFF;
    } else if (page == 20) {
        return emuState.videoRam[addr];
    } else if (page == 21) {
        if (addr < 0x800) {
            return emuState.charRam[addr];
        }
    } else if (page >= 32 && page < 64) {
        return emuState.mainRam[(page - 32) * 0x4000 + addr];
    }
    return 0xFF;
}

static void mem_write(size_t param, uint16_t addr, uint8_t data) {
    (void)param;

    // Handle CPM remap bit
    if (emuState.cpmRemap) {
        if (addr < 0x4000)
            addr += 0xC000;
        if (addr >= 0xC000)
            addr -= 0xC000;
    }

    // Get and decode banking register
    uint8_t  bankreg     = emuState.bankRegs[addr >> 14];
    unsigned page        = bankreg & 0x3F;
    bool     overlay_ram = (bankreg & (1 << 6)) != 0;
    bool     readonly    = (bankreg & (1 << 7)) != 0;
    addr &= 0x3FFF;

    if (overlay_ram && addr >= 0x3000) {
        if (addr < 0x3400) {
            emuState.screenRam[addr & 0x3FF] = data;
        } else if (addr < 0x3800) {
            emuState.colorRam[addr & 0x3FF] = data;
        } else {
            emuState.mainRam[addr] = data;
        }
        return;
    }

    if (readonly) {
        return;
    }

    if (page < 16) {
        // Flash memory is read-only for now
        // TODO: Implement flash emulation
        return;
    } else if (page == 19) {
        // Game ROM is readonly
        return;
    } else if (page == 20) {
        emuState.videoRam[addr] = data;
    } else if (page == 21) {
        if (addr < 0x800) {
            emuState.charRam[addr] = data;
        }
    } else if (page >= 32 && page < 64) {
        emuState.mainRam[(page - 32) * 0x4000 + addr] = data;
    }
}

static uint8_t io_read(size_t param, ushort addr) {
    (void)param;

    if (!emuState.sysCtrlDisableExt) {
        switch (addr & 0xFF) {
            case 0xE0: return emuState.videoCtrl;
            case 0xE1: return emuState.videoScrX & 0xFF;
            case 0xE2: return emuState.videoScrX >> 8;
            case 0xE3: return emuState.videoScrY;
            case 0xE4: return emuState.videoSprSel;
            case 0xE5: return emuState.videoSprX[emuState.videoSprSel] & 0xFF;
            case 0xE6: return emuState.videoSprX[emuState.videoSprSel] >> 8;
            case 0xE7: return emuState.videoSprY[emuState.videoSprSel];
            case 0xE8: return emuState.videoSprIdx[emuState.videoSprSel] & 0xFF;
            case 0xE9: return (
                (emuState.videoSprAttr[emuState.videoSprSel] & 0xFE) |
                ((emuState.videoSprIdx[emuState.videoSprSel] >> 8) & 1));
            case 0xEA: return emuState.videoPalSel;
            case 0xEB: return (emuState.videoPalette[emuState.videoPalSel >> 1] >> ((emuState.videoPalSel & 1) * 8)) & 0xFF;
            case 0xEC: return emuState.videoLine < 255 ? emuState.videoLine : 255;
            case 0xED: return emuState.videoIrqLine;
            case 0xEE: return emuState.irqMask;
            case 0xEF: return emuState.irqStatus;
            case 0xF0: return emuState.bankRegs[0];
            case 0xF1: return emuState.bankRegs[1];
            case 0xF2: return emuState.bankRegs[2];
            case 0xF3: return emuState.bankRegs[3];
            case 0xF4: return AqUartProtocol::instance().readCtrl();
            case 0xF5: return AqUartProtocol::instance().readData();
        }
    }

    switch (addr & 0xFF) {
        case 0xF6:
        case 0xF7:
            if (emuState.sysCtrlAyDisable)
                return 0xFF;
            else {
                switch (emuState.ay1Addr) {
                    case 14: return emuState.handCtrl1;
                    case 15: return emuState.handCtrl2;
                    default: return emuState.ay1.readReg(emuState.ay1Addr);
                }
            }
            break;

        case 0xF8:
        case 0xF9:
            if (emuState.sysCtrlAyDisable || emuState.sysCtrlDisableExt)
                return 0xFF;
            else
                return emuState.ay2.readReg(emuState.ay1Addr);

        case 0xFB: return (
            (emuState.sysCtrlDisableExt ? (1 << 0) : 0) |
            (emuState.sysCtrlAyDisable ? (1 << 1) : 0));

        case 0xFC: /* printf("Cassette port input (%04x)\n", addr); */ return 0xFF;
        case 0xFD: return (emuState.videoLine >= 242) ? 0 : 1;
        case 0xFE: /* printf("Clear to send status (%04x)\n", addr); */ return 0xFF;
        case 0xFF: {
            // Keyboard matrix. Selected rows are passed in the upper 8 address lines.
            uint8_t rows = addr >> 8;

            // Wire-AND all selected rows.
            uint8_t result = 0xFF;
            for (int i = 0; i < 8; i++) {
                if ((rows & (1 << i)) == 0) {
                    result &= emuState.keybMatrix[i];
                }
            }
            return result;
        }
        default: break;
    }

    printf("io_read(0x%02x)\n", addr & 0xFF);
    return 0xFF;
}

static void io_write(size_t param, uint16_t addr, uint8_t data) {
    (void)param;

    if (!emuState.sysCtrlDisableExt) {
        switch (addr & 0xFF) {
            case 0xE0: emuState.videoCtrl = data; return;
            case 0xE1: emuState.videoScrX = (emuState.videoScrX & ~0xFF) | data; return;
            case 0xE2: emuState.videoScrX = (emuState.videoScrX & 0xFF) | ((data & 1) << 8); return;
            case 0xE3: emuState.videoScrY = data; return;
            case 0xE4: emuState.videoSprSel = data & 0x3F; return;
            case 0xE5: emuState.videoSprX[emuState.videoSprSel] = (emuState.videoSprX[emuState.videoSprSel] & ~0xFF) | data; return;
            case 0xE6: emuState.videoSprX[emuState.videoSprSel] = (emuState.videoSprX[emuState.videoSprSel] & 0xFF) | ((data & 1) << 8); return;
            case 0xE7: emuState.videoSprY[emuState.videoSprSel] = data; return;
            case 0xE8: emuState.videoSprIdx[emuState.videoSprSel] = (emuState.videoSprIdx[emuState.videoSprSel] & ~0xFF) | data; return;
            case 0xE9:
                emuState.videoSprAttr[emuState.videoSprSel] = data & 0xFE;
                emuState.videoSprIdx[emuState.videoSprSel]  = (emuState.videoSprIdx[emuState.videoSprSel] & 0xFF) | ((data & 1) << 8);
                return;
            case 0xEA: emuState.videoPalSel = data & 0x7F; return;
            case 0xEB:
                if ((emuState.videoPalSel & 1) == 0) {
                    emuState.videoPalette[emuState.videoPalSel >> 1] =
                        (emuState.videoPalette[emuState.videoPalSel >> 1] & 0xF00) | data;
                } else {
                    emuState.videoPalette[emuState.videoPalSel >> 1] =
                        ((data & 0xF) << 8) | (emuState.videoPalette[emuState.videoPalSel >> 1] & 0xFF);
                }
                return;
            case 0xEC: return;
            case 0xED: emuState.videoIrqLine = data; return;
            case 0xEE: emuState.irqMask = data & 3; return;
            case 0xEF: emuState.irqStatus &= ~data; return;
            case 0xF0: emuState.bankRegs[0] = data; return;
            case 0xF1: emuState.bankRegs[1] = data; return;
            case 0xF2: emuState.bankRegs[2] = data; return;
            case 0xF3: emuState.bankRegs[3] = data; return;
            case 0xF4: AqUartProtocol::instance().writeCtrl(data); return;
            case 0xF5: AqUartProtocol::instance().writeData(data); return;
        }
    }

    switch (addr & 0xFF) {
        case 0xF6:
            if (!emuState.sysCtrlAyDisable && emuState.ay1Addr < 14)
                emuState.ay1.writeReg(emuState.ay1Addr, data);
            return;

        case 0xF7:
            if (!emuState.sysCtrlAyDisable)
                emuState.ay1Addr = data;
            return;

        case 0xF8:
            if (!(emuState.sysCtrlAyDisable || emuState.sysCtrlDisableExt) && emuState.ay2Addr < 14)
                emuState.ay2.writeReg(emuState.ay2Addr, data);
            return;

        case 0xF9:
            if (!(emuState.sysCtrlAyDisable || emuState.sysCtrlDisableExt))
                emuState.ay2Addr = data;
            return;

        case 0xFB:
            emuState.sysCtrlDisableExt = (data & (1 << 0)) != 0;
            emuState.sysCtrlAyDisable  = (data & (1 << 1)) != 0;
            return;

        case 0xFC: emuState.soundOutput = (data & 1) != 0; break;
        case 0xFD: emuState.cpmRemap = (data & 1) != 0; break;
        case 0xFE: /* printf("1200 bps serial printer (%04x) = %u\n", addr, data & 1); */ break;
        case 0xFF: break;
        default: printf("io_write(0x%02x, 0x%02x)\n", addr & 0xFF, data); break;
    }
}

void reset(void) {
    Z80RESET(&emuState.z80ctx);
    emuState.z80ctx.ioRead   = io_read;
    emuState.z80ctx.ioWrite  = io_write;
    emuState.z80ctx.memRead  = mem_read;
    emuState.z80ctx.memWrite = mem_write;

    emuState.cpmRemap = false;

    emuState.ay1.reset();
    emuState.ay2.reset();
}

// 3579545 Hz -> 59659 cycles / frame
// 7159090 Hz -> 119318 cycles / frame

// 455x262=119210 -> 60.05 Hz
// 51.2us + 1.5us + 4.7us + 6.2us = 63.6 us
// 366 active pixels

#define HCYCLES_PER_LINE (455)
#define HCYCLES_PER_SAMPLE (162)

static SDL_Texture *texture = NULL;

static void render_screen(SDL_Renderer *renderer) {
    if (texture == NULL) {
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VIDEO_WIDTH, VIDEO_HEIGHT);
    }

    void *pixels;
    int   pitch;
    SDL_LockTexture(texture, NULL, &pixels, &pitch);

    const uint16_t *fb = Video::instance().getFb();

    for (int j = 0; j < VIDEO_HEIGHT; j++) {
        for (int i = 0; i < VIDEO_WIDTH; i++) {

            // Convert from RGB444 to RGB888
            uint16_t col444 = fb[j * VIDEO_WIDTH + i];

            unsigned r4 = (col444 >> 8) & 0xF;
            unsigned g4 = (col444 >> 4) & 0xF;
            unsigned b4 = (col444 >> 0) & 0xF;

            unsigned r8 = (r4 << 4) | r4;
            unsigned g8 = (g4 << 4) | g4;
            unsigned b8 = (b4 << 4) | b4;

            ((uint32_t *)((uintptr_t)pixels + j * pitch))[i] = (r8 << 16) | (g8 << 8) | (b8);
        }
    }

    SDL_UnlockTexture(texture);
}

static void renderTexture(SDL_Renderer *renderer) {
    float rsx, rsy;
    SDL_RenderGetScale(renderer, &rsx, &rsy);
    auto  drawData = ImGui::GetDrawData();
    float scaleX   = (rsx == 1.0f) ? drawData->FramebufferScale.x : 1.0f;
    float scaleY   = (rsy == 1.0f) ? drawData->FramebufferScale.y : 1.0f;
    int   w        = (int)(drawData->DisplaySize.x * scaleX);
    int   h        = (int)(drawData->DisplaySize.y * scaleY);
    if (w <= 0 || h <= 0)
        return;

    // Retain aspect ratio
    int w1 = (w / VIDEO_WIDTH) * VIDEO_WIDTH;
    int h1 = (w1 * VIDEO_HEIGHT) / VIDEO_WIDTH;
    int h2 = (h / VIDEO_HEIGHT) * VIDEO_HEIGHT;
    int w2 = (h2 * VIDEO_WIDTH) / VIDEO_HEIGHT;

    int sw, sh;
    if (w1 == 0 || h1 == 0) {
        sw = w;
        sh = h;
    } else if (w1 <= w && h1 <= h) {
        sw = w1;
        sh = h1;
    } else {
        sw = w2;
        sh = h2;
    }

    SDL_Rect dst;
    dst.w = (int)sw;
    dst.h = (int)sh;
    dst.x = (w - dst.w) / 2;
    dst.y = (h - dst.h) / 2;
    SDL_RenderCopy(renderer, texture, NULL, &dst);
}

static void keyboard_type_in(void) {
    if (emuState.typeInRelease > 0) {
        emuState.typeInRelease--;
        if (emuState.typeInRelease > 0)
            return;
        AqKeyboard::instance().pressKey(emuState.typeInChar, false);
        emuState.typeInDelay = 1;
        return;
    }

    if (emuState.typeInDelay > 0) {
        emuState.typeInDelay--;
        if (emuState.typeInDelay > 0)
            return;
    }

    if (emuState.typeInStr == NULL)
        return;

    char ch = *(emuState.typeInStr++);
    if (ch == '\\') {
        ch = *(emuState.typeInStr++);
        if (ch == 'n') {
            ch = '\n';
        }
    }
    if (ch == 0) {
        emuState.typeInStr = NULL;
        return;
    }
    emuState.typeInChar = ch;
    AqKeyboard::instance().pressKey(emuState.typeInChar, true);
    emuState.typeInRelease = ch == '\n' ? 10 : 1;
}

static void emulate(SDL_Renderer *renderer) {
    // Emulation is performed in sync with the audio. This function will run
    // for the time needed to fill 1 audio buffer, which is about 1/60 of a
    // second.

    // Get a buffer from audio subsystem.
    auto abuf = Audio::instance().getBuffer();
    if (abuf == NULL) {
        // No buffer available, don't emulate for now.
        return;
    }

    // Render each audio sample
    for (int aidx = 0; aidx < SAMPLES_PER_BUFFER; aidx++) {
        do {
            emuState.z80ctx.tstates = 0;
            Z80Execute(&emuState.z80ctx);
            int delta = emuState.z80ctx.tstates * 2;

            int old_line_hcycles = emuState.lineHalfCycles;

            emuState.lineHalfCycles += delta;
            emuState.sampleHalfCycles += delta;

            if (old_line_hcycles < 320 && emuState.lineHalfCycles >= 320 && emuState.videoLine == emuState.videoIrqLine) {
                emuState.irqStatus |= (1 << 1);
            }

            if (emuState.lineHalfCycles >= HCYCLES_PER_LINE) {
                emuState.lineHalfCycles -= HCYCLES_PER_LINE;

                Video::instance().drawLine();

                emuState.videoLine++;
                if (emuState.videoLine == 200) {
                    emuState.irqStatus |= (1 << 0);

                } else if (emuState.videoLine == 262) {
                    render_screen(renderer);
                    emuState.videoLine = 0;

                    keyboard_type_in();
                }
            }

            if ((emuState.irqStatus & emuState.irqMask) != 0) {
                Z80INT(&emuState.z80ctx, 0xFF);
            }
        } while (emuState.sampleHalfCycles < HCYCLES_PER_SAMPLE);

        emuState.sampleHalfCycles -= HCYCLES_PER_SAMPLE;

        uint16_t beep = emuState.soundOutput ? AUDIO_LEVEL : 0;

        // Take average of 5 AY8910 samples to match sampling rate (16*5*44100 = 3.528MHz)
        unsigned left  = 0;
        unsigned right = 0;

        for (int i = 0; i < 5; i++) {
            uint16_t abc[3];
            emuState.ay1.render(abc);
            left += 2 * abc[0] + 2 * abc[1] + 1 * abc[2];
            right += 1 * abc[0] + 2 * abc[1] + 2 * abc[2];

            emuState.ay2.render(abc);
            left += 2 * abc[0] + 2 * abc[1] + 1 * abc[2];
            right += 1 * abc[0] + 2 * abc[1] + 2 * abc[2];
        }

        left  = left + beep;
        right = right + beep;

        abuf[aidx * 2 + 0] = left;
        abuf[aidx * 2 + 1] = right;
    }

    // Return buffer to audio subsystem.
    Audio::instance().putBuffer(abuf);
}

static void showAboutWindow(bool *p_open) {
    if (ImGui::Begin("About Aquarius+ emulator", p_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The Aquarius+ emulator is part of the Aquarius+ project.");
        ImGui::Text("Developed by Frank van den Hoef.\n");
        ImGui::Separator();
        ImGui::Text("Members of the Aquarius+ project team:");
        ImGui::Text("- Frank van den Hoef");
        ImGui::Text("- Sean P. Harrington");
        ImGui::Text("- Curtis F. Kaylor");
        ImGui::Separator();
        ImGui::Text(
            "Thanks go out all the people contributing to this project and\n"
            "those who enjoy playing with it, either with this emulator or\n"
            "the actual hardware!");
    }
    ImGui::End();
}

bool loadCartridgeROM(const char *path) {
    auto ifs = std::ifstream(path, std::ifstream::binary);
    if (!ifs.good()) {
        return false;
    }

    ifs.seekg(0, ifs.end);
    auto fileSize = ifs.tellg();
    ifs.seekg(0, ifs.beg);

    if (fileSize == 8192) {
        ifs.read((char *)(emuState.cartRom + 8192), fileSize);
        // Mirror ROM to $C000
        memcpy(emuState.cartRom, emuState.cartRom + 8192, 8192);

    } else if (fileSize == 16384) {
        ifs.read((char *)emuState.cartRom, fileSize);

    } else {
        fprintf(stderr, "Invalid cartridge ROM file: %u, should be either exactly 8 or 16KB.\n", (unsigned)fileSize);
        return false;
    }
    ifs.close();

    emuState.cartridgeInserted = true;

    return true;
}

int main(int argc, char *argv[]) {
    std::string basePath = SDL_GetBasePath();
    stripTrailingSlashes(basePath);

    std::string romPath = basePath + "/aquarius.rom";
    std::string cartRomPath;
    std::string sdCardPath;

    int  opt;
    bool paramsOk = true;
    bool showHelp = false;
    while ((opt = getopt(argc, argv, "hr:c:u:t:")) != -1) {
        if (opt == '?' || opt == ':') {
            paramsOk = false;
            break;
        }
        switch (opt) {
            case 'h': showHelp = true; break;
            case 'r': romPath = optarg; break;
            case 'c': cartRomPath = optarg; break;
            case 'u': sdCardPath = optarg; break;
            case 't': emuState.typeInStr = optarg; break;
            default: paramsOk = false; break;
        }
    }
    stripTrailingSlashes(sdCardPath);

    if (optind != argc || showHelp) {
        paramsOk = false;
    }
    if (!paramsOk) {
        sdCardPath.clear();
    }

#ifdef __APPLE__
    if (sdCardPath.empty()) {
        std::string homeDir = getpwuid(getuid())->pw_dir;
        sdCardPath          = homeDir + "/Documents/AquariusPlusDisk";
        mkdir(sdCardPath.c_str(), 0755);
    }
#elif _WIN32
    if (sdCardPath.empty()) {
        // PWSTR path = NULL;
        // char  path2[MAX_PATH];
        // SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &path);
        // WideCharToMultiByte(CP_UTF8, 0, path, -1, path2, sizeof(path2), NULL, NULL);
        // CoTaskMemFree(path);

        // sdCardPath = std::string(path2) + "/AquariusPlusDisk";
        sdCardPath = basePath + "/sdcard";
    }
    mkdir(sdCardPath.c_str());
#else
    if (sdCardPath.empty()) {
        sdCardPath = basePath + "/sdcard";
    }
#endif

    if (!paramsOk) {
        fprintf(stderr, "Usage: %s <options>\n\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "-h          This help screen\n");
        fprintf(stderr, "-r <path>   Set system ROM image path (default: %s/aquarius.rom)\n", basePath.c_str());
        fprintf(stderr, "-c <path>   Set cartridge ROM path\n");
        fprintf(stderr, "-u <path>   SD card base path (default: %s)\n", sdCardPath.c_str());
        fprintf(stderr, "-t <string> Type in string.\n");
        fprintf(stderr, "\n");
        exit(1);
    }

    AqUartProtocol::instance().init();
    SDCardVFS::instance().init(sdCardPath.c_str());

    emustate_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        SDL_Log("Unable to initialize SDL: %s", SDL_GetError());
        exit(1);
    }

    // Load system ROM
    {
        FILE *f = fopen(romPath.c_str(), "rb");
        if (f == NULL) {
            perror(romPath.c_str());
            exit(1);
        }
        if ((emuState.flashRomSize = fread(emuState.flashRom, 1, sizeof(emuState.flashRom), f)) < 8192) {
            fprintf(stderr, "Error during reading of system ROM image.\n");
            exit(1);
        }
        fclose(f);

        emuState.flashRomSize = (emuState.flashRomSize + 0x1FFF) & ~0x1FFF;
    }

    // Load cartridge ROM
    if (!cartRomPath.empty()) {
        if (!loadCartridgeROM(cartRomPath.c_str())) {
            fprintf(stderr, "Error loading cartridge ROM\n");
        }
    }

    unsigned long wnd_width = VIDEO_WIDTH * 2, wnd_height = VIDEO_HEIGHT * 2;
    SDL_Window   *window = SDL_CreateWindow("Aquarius+ emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, wnd_width, wnd_height, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        printf("SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        printf("SDL_CreateRenderer Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io    = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Audio::instance().init();
    reset();
    Audio::instance().start();
    AqKeyboard::instance().init();

    emuState.typeInRelease = 10;

    bool showDemoWindow      = false;
    bool showAppAbout        = false;
    bool showScreenWindow    = false;
    bool showRegistersWindow = false;

    MemoryEditor memEdit;
    memEdit.Open = false;

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            switch (event.type) {
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    // Don't pass keypresses to emulator when ImGUI has keyboard focus
                    if (io.WantCaptureKeyboard)
                        break;

                    if (!event.key.repeat && event.key.keysym.scancode <= 255) {
                        AqKeyboard::instance().handleScancode(event.key.keysym.scancode, event.type == SDL_KEYDOWN);
                        AqKeyboard::instance().updateMatrix();
                    }
                    break;
                }
                case SDL_USEREVENT: emulate(renderer); break;

                default:
                    if (event.type == SDL_QUIT)
                        done = true;
                    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                        done = true;
                    break;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("System")) {
                if (ImGui::MenuItem("Load cartridge ROM...", "")) {
                    char const *lFilterPatterns[1] = {"*.rom"};
                    char       *romFile            = tinyfd_openFileDialog("Open ROM file", "", 1, lFilterPatterns, "ROM files", 0);
                    if (romFile) {
                        if (loadCartridgeROM(romFile)) {
                            reset();
                        }
                    }
                }
                if (ImGui::MenuItem("Eject cartridge", "", false, emuState.cartridgeInserted)) {
                    emuState.cartridgeInserted = false;
                    reset();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Aquarius+", "")) {
                    reset();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "")) {
                    done = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug")) {
                if (ImGui::MenuItem("Screen in window", "")) {
                    showScreenWindow = true;
                }
                if (ImGui::MenuItem("Memory editor", "")) {
                    memEdit.Open = true;
                }
                if (ImGui::MenuItem("Registers", "")) {
                    showRegistersWindow = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("About", ""))
                    showAppAbout = true;
                ImGui::Separator();
                if (ImGui::MenuItem("ImGui library demo window", ""))
                    showDemoWindow = true;

                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (showAppAbout)
            showAboutWindow(&showAppAbout);
        if (showDemoWindow)
            ImGui::ShowDemoWindow(&showDemoWindow);
        if (memEdit.Open) {
            void      *pMem            = emuState.colorRam;
            size_t     memSize         = sizeof(emuState.colorRam);
            size_t     baseDisplayAddr = 0;
            static int itemCurrent     = 0;
            switch (itemCurrent) {
                case 0:
                    pMem    = emuState.screenRam;
                    memSize = sizeof(emuState.screenRam);
                    break;
                case 1:
                    pMem    = emuState.colorRam;
                    memSize = sizeof(emuState.colorRam);
                    break;
                case 2:
                    pMem    = emuState.flashRom;
                    memSize = emuState.flashRomSize;
                    break;
                case 3:
                    pMem    = emuState.mainRam;
                    memSize = sizeof(emuState.mainRam);
                    break;
                case 4:
                    pMem    = emuState.cartRom;
                    memSize = sizeof(emuState.cartRom);
                    break;
                case 5:
                    pMem    = emuState.videoRam;
                    memSize = sizeof(emuState.videoRam);
                    break;
                case 6:
                    pMem    = emuState.charRam;
                    memSize = sizeof(emuState.charRam);
                    break;
            }

            MemoryEditor::Sizes s;
            memEdit.CalcSizes(s, memSize, baseDisplayAddr);
            ImGui::SetNextWindowSize(ImVec2(s.WindowWidth, s.WindowWidth * 0.60f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(s.WindowWidth, FLT_MAX));

            memEdit.Open = true;
            if (ImGui::Begin("Memory editor", &memEdit.Open, ImGuiWindowFlags_NoScrollbar)) {
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    ImGui::OpenPopup("context");

                ImGui::Combo(
                    "Memory select", &itemCurrent,
                    "Screen RAM\0"
                    "Color RAM\0"
                    "System ROM\0"
                    "Main RAM\0"
                    "Cartridge ROM\0"
                    "Video RAM\0"
                    "Character RAM\0");

                ImGui::Separator();

                memEdit.DrawContents(pMem, memSize, baseDisplayAddr);
                if (memEdit.ContentsWidthChanged) {
                    memEdit.CalcSizes(s, memSize, baseDisplayAddr);
                    ImGui::SetWindowSize(ImVec2(s.WindowWidth, ImGui::GetWindowSize().y));
                }
            }
            ImGui::End();
        }

        if (showScreenWindow) {
            // ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            bool open = ImGui::Begin("Screen", &showScreenWindow, ImGuiWindowFlags_AlwaysAutoResize);
            // ImGui::PopStyleVar();

            if (open) {
                static int e = 1;
                ImGui::RadioButton("1x", &e, 1);
                ImGui::SameLine();
                ImGui::RadioButton("2x", &e, 2);
                ImGui::SameLine();
                ImGui::RadioButton("3x", &e, 3);
                ImGui::SameLine();
                ImGui::RadioButton("4x", &e, 4);

                if (texture) {
                    ImGui::Image((ImTextureID)(intptr_t)texture, ImVec2(VIDEO_WIDTH * e, VIDEO_HEIGHT * e));
                }
            }

            ImGui::End();
        }

        if (showRegistersWindow) {
            bool open = ImGui::Begin("Registers", &showRegistersWindow, ImGuiWindowFlags_AlwaysAutoResize);
            if (open) {
                ImGui::Text("PC: %04X", emuState.z80ctx.PC);
                ImGui::Separator();
                ImGui::Text(
                    "S:%u Z:%u H:%u PV:%u N:%u C:%u",
                    (emuState.z80ctx.R1.br.F & F_S) ? 1 : 0,
                    (emuState.z80ctx.R1.br.F & F_Z) ? 1 : 0,
                    (emuState.z80ctx.R1.br.F & F_H) ? 1 : 0,
                    (emuState.z80ctx.R1.br.F & F_PV) ? 1 : 0,
                    (emuState.z80ctx.R1.br.F & F_N) ? 1 : 0,
                    (emuState.z80ctx.R1.br.F & F_C) ? 1 : 0);
                ImGui::Separator();

                ImGui::BeginGroup();
                ImGui::Text("A : %02X", emuState.z80ctx.R1.br.A);
                ImGui::Text("F : %02X", emuState.z80ctx.R1.br.F);
                ImGui::Text("BC: %04X", emuState.z80ctx.R1.wr.BC);
                ImGui::Text("DE: %04X", emuState.z80ctx.R1.wr.DE);
                ImGui::Text("HL: %04X", emuState.z80ctx.R1.wr.HL);
                ImGui::Text("IX: %04X", emuState.z80ctx.R1.wr.IX);
                ImGui::Text("IY: %04X", emuState.z80ctx.R1.wr.IY);
                ImGui::Text("SP: %04X", emuState.z80ctx.R1.wr.SP);
                ImGui::EndGroup();
                ImGui::SameLine(0, 30);
                ImGui::BeginGroup();
                ImGui::Text("A' : %02X", emuState.z80ctx.R2.br.A);
                ImGui::Text("F' : %02X", emuState.z80ctx.R2.br.F);
                ImGui::Text("BC': %04X", emuState.z80ctx.R2.wr.BC);
                ImGui::Text("DE': %04X", emuState.z80ctx.R2.wr.DE);
                ImGui::Text("HL': %04X", emuState.z80ctx.R2.wr.HL);
                ImGui::Text("IX': %04X", emuState.z80ctx.R2.wr.IX);
                ImGui::Text("IY': %04X", emuState.z80ctx.R2.wr.IY);
                ImGui::Text("SP': %04X", emuState.z80ctx.R2.wr.SP);
                ImGui::EndGroup();
            }
            ImGui::End();
        }

        ImGui::Render();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (!showScreenWindow) {
            renderTexture(renderer);
        }

        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
