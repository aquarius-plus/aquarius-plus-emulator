#pragma once

#include "Common.h"
#include "z80.h"
#include "AqpVideo.h"
#include "AY8910.h"
#include <deque>

class _EmuState {
public:
    static _EmuState *get();

    virtual void spiSel(bool enable)                    = 0;
    virtual void spiTx(const void *data, size_t length) = 0;
    virtual void spiRx(void *buf, size_t length)        = 0;
};

// 3579545 Hz -> 59659 cycles / frame
// 7159090 Hz -> 119318 cycles / frame

// 455x262=119210 -> 60.05 Hz
// 51.2us + 1.5us + 4.7us + 6.2us = 63.6 us
// 366 active pixels

#define HCYCLES_PER_LINE   (455)
#define HCYCLES_PER_SAMPLE (162)

enum EmulateResultFlags {
    ERF_RENDER_SCREEN    = (1 << 0),
    ERF_NEW_AUDIO_SAMPLE = (1 << 1),
};

struct EmuState;
extern EmuState emuState;

struct EmuState {
    EmuState();
    void coldReset();
    void warmReset();
    bool loadCartridgeROM(const std::string &path);
    void keyboardTypeIn();

    static uint8_t _memRead(size_t param, uint16_t addr) {
        (void)param;
        return emuState.memRead(addr, true);
    }
    static void _memWrite(size_t param, uint16_t addr, uint8_t data) {
        (void)param;
        return emuState.memWrite(addr, data, true);
    }
    static uint8_t _ioRead(size_t param, uint16_t addr) {
        (void)param;
        return emuState.ioRead(addr, true);
    }
    static void _ioWrite(size_t param, uint16_t addr, uint8_t data) {
        (void)param;
        return emuState.ioWrite(addr, data, true);
    }

    uint8_t memRead(uint16_t addr, bool triggerBp = false);
    void    memWrite(uint16_t addr, uint8_t data, bool triggerBp = false);
    uint8_t ioRead(uint16_t addr, bool triggerBp = false);
    void    ioWrite(uint16_t addr, uint8_t data, bool triggerBp = false);

    unsigned emulate();
    int      cpuEmulate();
    unsigned audioLeft  = 0;
    unsigned audioRight = 0;

    // Debugger
    bool enableDebugger    = false;
    bool enableBreakpoints = false;

    struct Breakpoint {
        uint16_t    addr = 0;
        std::string name;
        bool        enabled = false;
        int         type    = 0;
        bool        onR     = true;
        bool        onW     = true;
        bool        onX     = true;
    };

    std::vector<Breakpoint> breakpoints;
    int                     tmpBreakpoint = -1;
    int                     lastBpAddress = -1;
    int                     lastBp        = -1;
    int                     haltAfterRet  = -1;

    enum class WatchType {
        Hex8 = 0,
        DecU8,
        DecS8,
        Hex16,
        DecU16,
        DecS16,
    };
    struct Watch {
        uint16_t    addr = 0;
        std::string name;
        WatchType   type = WatchType::Hex8;
    };
    std::vector<Watch> watches;

    enum EmuMode {
        Em_Halted,
        Em_Step,
        Em_Running,
    };
    EmuMode emuMode = Em_Running;

    // Emulator state
    Z80Context z80ctx;                  // Z80 emulation core state
    AqpVideo   video;                   // Video
    int        lineHalfCycles    = 0;   // Half-cycles for this line
    int        sampleHalfCycles  = 0;   // Half-cycles for this sample
    uint8_t    keybMatrix[8]     = {0}; // Keyboard matrix (8 x 6bits)
    bool       cartridgeInserted = false;

    // Keyboard buffer
    uint8_t kbBuf[16];
    uint8_t kbBufWrIdx = 0;
    uint8_t kbBufRdIdx = 0;
    uint8_t kbBufCnt   = 0;

    void    kbBufReset();
    void    kbBufWrite(uint8_t val);
    uint8_t kbBufRead();

    // CPU tracing
    struct Z80TraceEntry {
        Z80Regs  r1;
        Z80Regs  r2;
        uint16_t pc;
        char     bytes[32];
        char     instrStr[32];
    };
    std::deque<Z80TraceEntry> cpuTrace;
    bool                      traceEnable    = false;
    int                       traceDepth     = 128;
    bool                      prevHalted     = false;
    int                       emulationSpeed = 1;

    // Virtual typing from command-line argument
    std::string typeInStr;

    // Mouse state
    float mouseHideTimeout = 0;

    // Stop the CPU when a HALT
    // instruction is executed.
    bool stopOnHalt = false;

    // IO space
    uint8_t audioDAC    = 0;               // $EC   : Audio DAC sample
    uint8_t irqMask     = 0;               // $EE   : Interrupt mask register
    uint8_t irqStatus   = 0;               // $EF   : Interrupt status register
    uint8_t bankRegs[4] = {0};             // $F0-F3: Banking registers
    AY8910  ay1;                           // $F6/F7: AY-3-8910 emulation state
    AY8910  ay2;                           // $F8/F9: AY-3-8910 emulation state
    bool    sysCtrlDisableExt     = false; // $FB<0>: Disable access to extended registers
    bool    sysCtrlAyDisable      = false; // $FB<1>: Disable AY PSGs
    bool    sysCtrlTurbo          = false; // $FB<2>: Turbo mode
    bool    sysCtrlTurboUnlimited = false; // $FB<3>: Turbo unlimited mode
    bool    sysCtrlWarmBoot       = false; // $FB<7>: R0:Cold boot, R1:Warm boot
    bool    soundOutput           = false; // $FC<1>: Cassette/Sound output
    bool    cpmRemap              = false; // $FD<1>: Remap memory for CP/M

    // Memory space
    uint8_t mainRam[512 * 1024]; // Main RAM
    uint8_t cartRom[16 * 1024];  // Cartridge ROM
};
