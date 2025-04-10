#pragma once

#include "Common.h"
#include <SDL.h>
#include "cJSON.h"

enum class DisplayScaling {
    NearestNeighbor = 0,
    Linear,
    Integer,
};

class Config {
    Config();

public:
    static Config *instance();

    void init(const std::string &appDataPath);

    void load();
    void save();

    std::string appDataPath;
    std::string configPath;
    std::string imguiConf;
    std::string sdCardPath;
    std::string asmListingPath;

    int  wndPosX             = 0;
    int  wndPosY             = 0;
    int  wndWidth            = 0;
    int  wndHeight           = 0;
    bool enableSound         = true;
    bool enableMouse         = true;
    bool fontScale2x         = false;
    bool enableDebugger      = false;
    bool showMemEdit         = false;
    bool showCpuState        = false;
    bool showIoRegsWindow    = false;
    bool showBreakpoints     = false;
    bool showAssemblyListing = false;
    bool showCpuTrace        = false;
    bool showWatch           = false;
    bool showEspInfo         = false;
    bool stopOnHalt          = false;
    int  memEditMemSelect    = 0;

    DisplayScaling displayScaling = DisplayScaling::Linear;
};

static inline std::string getStringValue(cJSON *parent, const std::string &key, const std::string &defaultValue) {
    if (auto obj = cJSON_GetObjectItem(parent, key.c_str())) {
        auto value = cJSON_GetStringValue(obj);
        if (value)
            return value;
    }
    return defaultValue;
}

static inline bool getBoolValue(cJSON *parent, const std::string &key, bool defaultValue) {
    if (auto obj = cJSON_GetObjectItem(parent, key.c_str())) {
        if (cJSON_IsBool(obj)) {
            return cJSON_IsTrue(obj);
        }
    }
    return defaultValue;
}

static inline int getIntValue(cJSON *parent, const std::string &key, int defaultValue) {
    if (auto obj = cJSON_GetObjectItem(parent, key.c_str())) {
        if (cJSON_IsNumber(obj)) {
            return (int)cJSON_GetNumberValue(obj);
        }
    }
    return defaultValue;
}
