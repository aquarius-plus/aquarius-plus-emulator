#pragma once

#include "Common.h"

class Aq32Audio {
public:
    Aq32Audio();
    void reset();
    void render(int16_t result[2]);

    uint32_t reg0_ch_op4;
    uint32_t reg1;
    uint32_t reg2_kon;
    uint32_t ch_restart;
    uint32_t ch_attr[32];
    uint32_t op_attr0[64];
    uint32_t op_attr1[64];

    void dbgDrawIoRegs();

private:
    struct ChData {
        int16_t prevSamples[2];
    };
    struct OpData {
        uint32_t phase;
        uint8_t  stage;
        int      envCnt;
    };

    ChData chData[32];
    OpData opData[64];

    uint16_t timer;
    uint8_t  vibpos;
    bool     am_dir;
    uint8_t  am_cnt;
};
