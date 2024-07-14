#pragma once

#include "Common.h"
#include "VFS.h"

#define MAX_FDS (10)
#define MAX_DDS (10)

enum GameCtrlBtnIdx {
    GCB_A          = (1 << 0),
    GCB_B          = (1 << 1),
    GCB_X          = (1 << 2),
    GCB_Y          = (1 << 3),
    GCB_VIEW       = (1 << 4),
    GCB_GUIDE      = (1 << 5),
    GCB_MENU       = (1 << 6),
    GCB_LS         = (1 << 7),
    GCB_RS         = (1 << 8),
    GCB_LB         = (1 << 9),
    GCB_RB         = (1 << 10),
    GCB_DPAD_UP    = (1 << 11),
    GCB_DPAD_DOWN  = (1 << 12),
    GCB_DPAD_LEFT  = (1 << 13),
    GCB_DPAD_RIGHT = (1 << 14),
    GCB_SHARE      = (1 << 15),
};

class AqUartProtocol {
    AqUartProtocol();

public:
    static AqUartProtocol &instance();

    void init();

#ifdef EMULATOR
    void    writeCtrl(uint8_t data);
    void    writeData(uint8_t data);
    uint8_t readCtrl();
    uint8_t readData();
#endif

#ifndef EMULATOR
    void mouseReport(int dx, int dy, uint8_t buttonMask, int dWheel);
    void setMouseSensitivityDiv(uint8_t val) {
        mouseSensitivityDiv = std::max((uint8_t)1, std::min((uint8_t)8, val));
    }
    uint8_t getMouseSensitivityDiv() {
        return mouseSensitivityDiv;
    }
#endif

#ifdef EMULATOR
    struct FileInfo {
        uint8_t     flags;
        std::string name;
        unsigned    offset;
    };
    std::map<uint8_t, FileInfo> fi;

    struct DirInfo {
        std::string name;
        unsigned    offset;
    };
    std::map<uint8_t, DirInfo> di;
#endif

private:
#ifndef EMULATOR
    static void _uartEventTask(void *);
    void        uartEventTask();
#endif

#ifdef EMULATOR
    int txFifoRead();
#endif

    void        txFifoWrite(uint8_t data);
    void        txFifoWrite(const void *buf, size_t length);
    void        splitPath(const std::string &path, std::vector<std::string> &result);
    std::string resolvePath(std::string path, VFS **vfs, std::string *wildCard = nullptr);
    void        closeAllDescriptors();
    void        receivedByte(uint8_t data);

    void cmdReset();
    void cmdVersion();
    void cmdGetDateTime(uint8_t type);
    void cmdKeyMode(uint8_t mode);
    void cmdGetMouse();
    void cmdGetGameCtrl(uint8_t idx);
    void cmdOpen(uint8_t flags, const char *pathArg);
    void cmdClose(uint8_t fd);
    void cmdRead(uint8_t fd, uint16_t size);
    void cmdReadLine(uint8_t fd, uint16_t size);
    void cmdWrite(uint8_t fd, uint16_t size, const void *data);
    void cmdSeek(uint8_t fd, uint32_t offset);
    void cmdTell(uint8_t fd);
    void cmdOpenDirExt(const char *pathArg, uint8_t flags, uint16_t skip_count);
    void cmdCloseDir(uint8_t dd);
    void cmdReadDir(uint8_t dd);
    void cmdDelete(const char *pathArg);
    void cmdRename(const char *oldArg, const char *newArg);
    void cmdMkDir(const char *pathArg);
    void cmdChDir(const char *pathArg);
    void cmdStat(const char *pathArg);
    void cmdGetCwd();
    void cmdCloseAll();
    void cmdLoadFpga(const char *pathArg);

#ifndef EMULATOR
    QueueHandle_t uartQueue;
#endif
    std::string currentPath;
    uint8_t     rxBuf[16 + 0x10000];
    unsigned    rxBufIdx;
    VFS        *fdVfs[MAX_FDS];
    uint8_t     fds[MAX_FDS];
    DirEnumCtx  deCtxs[MAX_DDS];
    int         deIdx[MAX_DDS];
    const char *newPath;

#ifdef EMULATOR
    uint8_t  txBuf[0x10000 + 16];
    unsigned txBufWrIdx;
    unsigned txBufRdIdx;
    unsigned txBufCnt;
#endif

#ifndef EMULATOR
    SemaphoreHandle_t mutexMouseData;
    uint8_t           mouseSensitivityDiv = 4;
#endif
    bool    mousePresent = false;
    float   mouseX       = 0;
    float   mouseY       = 0;
    uint8_t mouseButtons = 0;
    int     mouseWheel   = 0;

#ifndef EMULATOR
    SemaphoreHandle_t mutexGameCtrlData;
#endif
    bool     gameCtrlPresent = false;
    int8_t   gameCtrlLX      = 0;
    int8_t   gameCtrlLY      = 0;
    int8_t   gameCtrlRX      = 0;
    int8_t   gameCtrlRY      = 0;
    uint8_t  gameCtrlLT      = 0;
    uint8_t  gameCtrlRT      = 0;
    uint16_t gameCtrlButtons = 0;

    void gameCtrlReset(bool present) {
        gameCtrlPresent = present;
        gameCtrlLX      = 0;
        gameCtrlLY      = 0;
        gameCtrlRX      = 0;
        gameCtrlRY      = 0;
        gameCtrlLT      = 0;
        gameCtrlRT      = 0;
        gameCtrlButtons = 0;
        gameCtrlUpdated();
    }

    void gameCtrlUpdated();

#ifdef EMULATOR
    friend class UI;
#endif
};
