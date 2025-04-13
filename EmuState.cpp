#include "EmuState.h"
#include <stdlib.h>
#include "UartProtocol.h"
#include "Keyboard.h"
#include "fpgarom.h"
#include "FpgaCore.h"
#include "AqpEmuState.h"

static std::shared_ptr<EmuState> curEmuState;

std::shared_ptr<EmuState> EmuState::get() {
    return curEmuState;
}

void EmuState::loadCore(const std::string &name) {
    printf("loadCore %s\n", name.c_str());

    curEmuState = nullptr;
    if (name == "aqplus.core") {
        curEmuState = std::make_shared<AqpEmuState>();
    }
}

void esp_restart() {
}
