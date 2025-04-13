#include "EmuState.h"
#include <stdlib.h>
#include "UartProtocol.h"
#include "Keyboard.h"
#include "fpgarom.h"
#include "FpgaCore.h"

static std::shared_ptr<EmuState> curEmuState;

std::shared_ptr<EmuState> EmuState::get() {
    return curEmuState;
}

void EmuState::loadCore(const std::string &_name) {
    curEmuState = nullptr;

    std::string name;

    // Normalize passed name
    {
        std::vector<std::string> result;
        splitPath(_name, result);
        if (result.empty())
            return;

        name = result.back();
        for (auto &ch : name) {
            ch = tolower(ch);
        }
    }

    printf("loadCore %s\n", name.c_str());
    if (name == "aqplus.core") {
        curEmuState = newAqpEmuState();
    } else if (name == "aqms.core") {
    }
}

void esp_restart() {
}
