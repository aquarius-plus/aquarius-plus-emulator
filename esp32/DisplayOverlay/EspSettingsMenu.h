#pragma once

#include "Menu.h"
#include "KeyboardLayoutMenu.h"
#include "Keyboard.h"
#ifndef EMULATOR
#include "WiFiMenu.h"
#include "BluetoothMenu.h"
#include "TimeZoneMenu.h"
#include "GitHubUpdateMenu.h"
#include "SdCardUpdateMenu.h"
#include "EspStatsMenu.h"
#include "FileServer.h"

static WiFiMenu      wifiMenu;
static BluetoothMenu btMenu;
#endif

//////////////////////////////////////////////////////////////////////////////
// Esp settings menu
//////////////////////////////////////////////////////////////////////////////
class EspSettingsMenu : public Menu {
public:
    EspSettingsMenu() : Menu("ESP settings", 38) {
    }

    void onUpdate() override {
        items.clear();
#ifndef EMULATOR
        {
            auto &item   = items.emplace_back(MenuItemType::subMenu, "Wi-Fi");
            item.onEnter = []() { wifiMenu.show(); };
        }
        {
            auto &item   = items.emplace_back(MenuItemType::subMenu, "Bluetooth");
            item.onEnter = []() { btMenu.show(); };
        }
        items.emplace_back(MenuItemType::separator);
        {
            auto &item  = items.emplace_back(MenuItemType::onOff, "File server");
            item.setter = [this](int newVal) {
                bool fileServerOn = (newVal != 0);
                if (getFileServer()->isRunning() != fileServerOn) {
                    if (fileServerOn)
                        getFileServer()->start();
                    else
                        getFileServer()->stop();

                    nvs_handle_t h;
                    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
                        if (nvs_set_u8(h, "fileserver", fileServerOn ? 1 : 0) == ESP_OK) {
                            nvs_commit(h);
                        }
                        nvs_close(h);
                    }
                }
            };
            item.getter = [this]() { return getFileServer()->isRunning() ? 1 : 0; };
        }
        {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "Time zone: %s", TimeZoneMenu::getTimeZone().c_str());

            auto &item   = items.emplace_back(MenuItemType::subMenu, tmp);
            item.onEnter = [&]() {
                TimeZoneMenu subMenu;
                subMenu.show();
                setNeedsUpdate();
            };
        }
#endif
        {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "Keyboard layout: %s", Keyboard::instance()->getKeyLayoutName(Keyboard::instance()->getKeyLayout()).c_str());

            auto &item   = items.emplace_back(MenuItemType::subMenu, tmp);
            item.onEnter = [&]() {
                KeyboardLayoutMenu subMenu;
                subMenu.show();
                setNeedsUpdate();
            };
        }
        items.emplace_back(MenuItemType::separator);
#ifndef EMULATOR
        {
            auto &item   = items.emplace_back(MenuItemType::subMenu, "System update from GitHub");
            item.onEnter = []() {
                GitHubUpdateMenu subMenu;
                subMenu.show();
            };
        }
        {
            auto &item   = items.emplace_back(MenuItemType::subMenu, "System update from SD card");
            item.onEnter = []() {
                SdCardUpdateMenu subMenu;
                subMenu.show();
            };
        }
#endif
        {
            auto &item   = items.emplace_back(MenuItemType::subMenu, "Factory reset");
            item.onEnter = [&]() {
                drawMessage("Are you sure? (Type 'y' to continue)");
                int ch = Keyboard::instance()->getKey(portMAX_DELAY);
                if (ch == 'y') {
                    drawMessage("Erasing settings...");
                    ESP_ERROR_CHECK(nvs_flash_erase());
                    ESP_ERROR_CHECK(nvs_flash_init());
                    SystemRestart();
                }
            };
        }
#ifndef EMULATOR
        items.emplace_back(MenuItemType::separator);
        {
            auto &item   = items.emplace_back(MenuItemType::subMenu, "ESP stats");
            item.onEnter = [&]() {
                EspStatsMenu subMenu;
                subMenu.show();
            };
        }
#endif
    }
};
