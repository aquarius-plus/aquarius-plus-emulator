#pragma once

#include "z80.h"
#include "AssemblyListing.h"
#include "Config.h"

class Z80Core {
public:
    Z80Core();

    std::function<bool()>                            hasIrq;
    std::function<uint8_t(uint16_t addr)>            memRead;
    std::function<void(uint16_t addr, uint8_t data)> memWrite;
    std::function<uint8_t(uint16_t addr)>            ioRead;
    std::function<void(uint16_t addr, uint8_t data)> ioWrite;
    std::function<void(uint16_t addr)>               showInMemEdit;

    void loadConfig(cJSON *root);
    void saveConfig(cJSON *root);
    int  emulate();
    void reset();
    void setEnableDebugger(bool en) {
        enableDebugger = en;
        if (!enableDebugger) {
            // Always run when not debugging
            emuMode = Em_Running;
        }
    }
    void pendIrq() { Z80INT(&z80ctx, 0xFF); } // used by aqms core

    void dbgMenu();
    void dbgWindows();

    int lastBpAddress = -1;
    int tmpBreakpoint = -1;
    int haltAfterRet  = -1;

private:
    // Z80 emulation core
    Z80Context z80ctx;

    static uint8_t _z80MemRead(uintptr_t param, uint16_t addr);
    static void    _z80MemWrite(uintptr_t param, uint16_t addr, uint8_t data);
    static uint8_t _z80IoRead(uintptr_t param, uint16_t addr);
    static void    _z80IoWrite(uintptr_t param, uint16_t addr, uint8_t data);

    uint8_t z80MemRead(uint16_t addr, bool triggerBp);
    void    z80MemWrite(uint16_t addr, uint8_t data, bool triggerBp);
    uint8_t z80IoRead(uint16_t addr, bool triggerBp);
    void    z80IoWrite(uint16_t addr, uint8_t data, bool triggerBp);

    // Current mode
    enum EmuMode {
        Em_Halted,
        Em_Step,
        Em_Running,
    };
    EmuMode emuMode = Em_Running;

    bool enableDebugger = false;

    // Breakpoints
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
    int                     lastBp = -1;

    // Assembly listing
    AssemblyListing asmListing;

    // Watches
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

    // Tracing
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

    // Debug UI
    bool showCpuState        = false;
    bool showBreakpoints     = false;
    bool showAssemblyListing = false;
    bool showCpuTrace        = false;
    bool showWatch           = false;
    bool stopOnHalt          = false; // Stop the CPU when a HALT instruction is executed.

    void dbgWndCpuState(bool *p_open);
    void dbgWndBreakpoints(bool *p_open);
    void dbgWndAssemblyListing(bool *p_open);
    void dbgWndCpuTrace(bool *p_open);
    void dbgWndWatch(bool *p_open);
    void addrPopup(uint16_t addr);
    void listingReloaded();
};
