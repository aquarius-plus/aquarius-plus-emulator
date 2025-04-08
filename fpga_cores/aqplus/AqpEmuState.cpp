#include "AqpEmuState.h"
#include "FPGA.h"

AqpEmuState::AqpEmuState() {
}

void AqpEmuState::spiSel(bool enable) {

    if (enabled == enable)
        return;
    enabled = enable;

    if (!enabled && !txBuf.empty()) {
        switch (txBuf[0]) {
            case CMD_RESET: {
                if (txBuf.size() >= 1 + 1) {
                    if (txBuf[1] & 2) {
                        emuState.coldReset();
                    } else {
                        emuState.warmReset();
                    }
                }
                break;
            }

            case CMD_SET_KEYB_MATRIX: {
                if (txBuf.size() >= 1 + 8) {
                    memcpy(&emuState.keybMatrix, &txBuf[1], 8);
                }
                break;
            }

            case CMD_SET_HCTRL: {
                if (txBuf.size() >= 1 + 2) {
                    emuState.ay1.portRdData[0] = txBuf[1];
                    emuState.ay1.portRdData[1] = txBuf[2];
                }
                break;
            }

            case CMD_WRITE_KBBUF: {
                if (txBuf.size() >= 1 + 1) {
                    emuState.kbBufWrite(txBuf[1]);
                }
                break;
            }
        }
    }

    txBuf.clear();
}

void AqpEmuState::spiTx(const void *data, size_t length) {
    if (!enabled)
        return;
    auto p = static_cast<const uint8_t *>(data);
    txBuf.insert(txBuf.end(), p, p + length);
}

void AqpEmuState::spiRx(void *buf, size_t length) {
    if (!enabled)
        return;
}
