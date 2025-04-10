#include "EmuState.h"
#include <stdlib.h>
#include "UartProtocol.h"
#include "Keyboard.h"
#include "fpgarom.h"
#include "FpgaCore.h"
#include "AqpEmuState.h"

AqpEmuState aqpEmuState;
EmuState   *EmuState::get() {
    return &aqpEmuState;
}

void esp_restart() {
}
