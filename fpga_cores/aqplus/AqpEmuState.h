#pragma once

#include "EmuState.h"
#include "AqpVideo.h"
#include "z80.h"
#include "DCBlock.h"

#include "AssemblyListing.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "MemoryEditor.h"

class AqpEmuState : public EmuState {
public:
    AqpEmuState();

    void loadConfig(cJSON *root) override;
    void saveConfig(cJSON *root) override;

    void reset(bool cold) override;
    bool emulate(int16_t *audioBuf, unsigned numSamples) override;
    void getPixels(void *pixels, int pitch) override;
    void spiSel(bool enable) override;
    void spiTx(const void *data, size_t length) override;
    void spiRx(void *buf, size_t length) override;

    void fileMenu() override;
    void pasteText(const std::string &str) override { typeInStr = str; }
    bool pasteIsDone() override { return typeInStr.empty(); }

    bool getDebuggerEnabled() override { return enableDebugger; }
    void setDebuggerEnabled(bool en) override { enableDebugger = en; }
    void dbgMenu() override;
    void dbgWindows() override;

private:
    enum EmuMode {
        Em_Halted,
        Em_Step,
        Em_Running,
    };

    uint8_t  ovlFont[2048];
    uint16_t ovlPalette[32];
    uint16_t ovlText[1024];

    unsigned emulate2();

    void dbgWndCpuState(bool *p_open);
    void dbgWndIoRegs(bool *p_open);
    void dbgWndBreakpoints(bool *p_open);
    void dbgWndMemEdit(bool *p_open);
    void dbgWndAssemblyListing(bool *p_open);
    void dbgWndCpuTrace(bool *p_open);
    void dbgWndWatch(bool *p_open);

    void listingReloaded();
    void addrPopup(uint16_t addr);

    AqpVideo video;                   // Video
    int      lineHalfCycles    = 0;   // Half-cycles for this line
    int      sampleHalfCycles  = 0;   // Half-cycles for this sample
    uint8_t  keybMatrix[8]     = {0}; // Keyboard matrix (8 x 6bits)
    bool     cartridgeInserted = false;

    unsigned audioLeft  = 0;
    unsigned audioRight = 0;
    DCBlock  dcBlockLeft;
    DCBlock  dcBlockRight;

    std::vector<uint8_t> txBuf;
    bool                 enabled = false;

    int cpuEmulate();

    bool loadCartridgeROM(const std::string &path);
    void keyboardTypeIn();

    // Virtual typing from command-line argument
    std::string typeInStr;

    // Stop the CPU when a HALT
    // instruction is executed.
    bool stopOnHalt = false;

    // Z80 emulation core
    Z80Context z80ctx;

    static uint8_t _memRead(uintptr_t param, uint16_t addr) {
        return reinterpret_cast<AqpEmuState *>(param)->memRead(addr, true);
    }
    static void _memWrite(uintptr_t param, uint16_t addr, uint8_t data) {
        reinterpret_cast<AqpEmuState *>(param)->memWrite(addr, data, true);
    }
    static uint8_t _ioRead(uintptr_t param, uint16_t addr) {
        return reinterpret_cast<AqpEmuState *>(param)->ioRead(addr, true);
    }
    static void _ioWrite(uintptr_t param, uint16_t addr, uint8_t data) {
        reinterpret_cast<AqpEmuState *>(param)->ioWrite(addr, data, true);
    }

    uint8_t memRead(uint16_t addr, bool triggerBp = false);
    void    memWrite(uint16_t addr, uint8_t data, bool triggerBp = false);
    uint8_t ioRead(uint16_t addr, bool triggerBp = false);
    void    ioWrite(uint16_t addr, uint8_t data, bool triggerBp = false);

    // Debugging
    bool enableDebugger    = false;
    bool enableBreakpoints = false;

    AssemblyListing asmListing;
    MemoryEditor    memEdit;

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

    // CPU tracing
    struct Z80TraceEntry {
        Z80Regs  r1;
        Z80Regs  r2;
        uint16_t pc;
        char     bytes[32];
        char     instrStr[32];
    };
    std::deque<Z80TraceEntry> cpuTrace;
    bool                      traceEnable = false;
    int                       traceDepth  = 128;
    bool                      prevHalted  = false;

    EmuMode emuMode = Em_Running;

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

    // Keyboard buffer
    uint8_t kbBuf[16];
    uint8_t kbBufWrIdx = 0;
    uint8_t kbBufRdIdx = 0;
    uint8_t kbBufCnt   = 0;

    void    kbBufReset();
    void    kbBufWrite(uint8_t val);
    uint8_t kbBufRead();

    // External memory
    uint8_t mainRam[512 * 1024]; // Main RAM
    uint8_t cartRom[16 * 1024];  // Cartridge ROM
};
