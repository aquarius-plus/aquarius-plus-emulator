#include "Config.h"
#include "EmuState.h"
#include "Keyboard.h"

Config::Config() {
}

Config *Config::instance() {
    static Config obj;
    return &obj;
}

void Config::init(const std::string &_appDataPath) {
    appDataPath = _appDataPath;
    load();
    save();
}

cJSON *Config::loadConfigFile(const std::string &filename) {
    std::string   jsonStr = "{}";
    std::ifstream ifs(appDataPath + "/" + filename);
    if (ifs.good()) {
        jsonStr = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    auto root = cJSON_ParseWithLength(jsonStr.c_str(), jsonStr.size());
    return root;
}

void Config::saveConfigFile(const std::string &filename, cJSON *root) {
    auto          path = appDataPath + "/" + filename;
    std::ofstream ofs(path);
    if (ofs.good()) {
        auto str = cJSON_Print(root);
        ofs.write(str, strlen(str));
        cJSON_free(str);
    }
    cJSON_Delete(root);
}

void Config::load() {
    if (auto root = loadConfigFile("config.json")) {
        imguiConf      = getStringValue(root, "imguiConfig", "");
        sdCardPath     = getStringValue(root, "sdCardPath", "");
        asmListingPath = getStringValue(root, "asmListingPath", "");

        wndPosX        = getIntValue(root, "wndPosX", SDL_WINDOWPOS_CENTERED);
        wndPosY        = getIntValue(root, "wndPosY", SDL_WINDOWPOS_CENTERED);
        wndWidth       = getIntValue(root, "wndWidth", 800);
        wndHeight      = getIntValue(root, "wndHeight", 600);
        enableSound    = getBoolValue(root, "enableSound", true);
        enableMouse    = getBoolValue(root, "enableMouse", true);
        fontScale2x    = getBoolValue(root, "fontScale2x", false);
        enableDebugger = getBoolValue(root, "enableDebugger", false);

        displayScaling = (DisplayScaling)getIntValue(root, "displayScaling", (int)DisplayScaling::Linear);

        Keyboard::instance()->setKeyLayout((KeyLayout)getIntValue(root, "keyLayout", 0));

        showMemEdit         = getBoolValue(root, "showMemEdit", false);
        showCpuState        = getBoolValue(root, "showCpuState", false);
        showIoRegsWindow    = getBoolValue(root, "showIoRegsWindow", false);
        showBreakpoints     = getBoolValue(root, "showBreakpoints", false);
        showAssemblyListing = getBoolValue(root, "showAssemblyListing", false);
        showCpuTrace        = getBoolValue(root, "showCpuTrace", false);
        showWatch           = getBoolValue(root, "showWatch", false);
        showEspInfo         = getBoolValue(root, "showEspInfo", false);
        stopOnHalt          = getBoolValue(root, "stopOnHalt", false);

        // Read all NVS U8 items
        {
            nvs_u8.clear();
            if (auto obj = cJSON_GetObjectItem(root, "nvs_u8")) {
                cJSON *item;
                cJSON_ArrayForEach(item, obj) {
                    if (item->type != cJSON_Number)
                        continue;

                    uint8_t val = item->valueint;
                    nvs_u8.insert_or_assign(item->string, val);
                }
            }
        }

        memEditMemSelect = getIntValue(root, "memEditMemSelect", 0);

        cJSON_free(root);
    }

    // Sanitize some variables
    wndPosX   = std::max(wndPosX, 0);
    wndPosY   = std::max(wndPosY, 0);
    wndWidth  = std::max(wndWidth, 800);
    wndHeight = std::max(wndHeight, 600);
}

void Config::save() {
    auto root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "imguiConfig", cJSON_CreateString(imguiConf.c_str()));
    cJSON_AddStringToObject(root, "sdCardPath", sdCardPath.c_str());
    cJSON_AddStringToObject(root, "asmListingPath", asmListingPath.c_str());

    cJSON_AddNumberToObject(root, "wndPosX", wndPosX);
    cJSON_AddNumberToObject(root, "wndPosY", wndPosY);
    cJSON_AddNumberToObject(root, "wndWidth", wndWidth);
    cJSON_AddNumberToObject(root, "wndHeight", wndHeight);
    cJSON_AddBoolToObject(root, "enableSound", enableSound);
    cJSON_AddBoolToObject(root, "enableMouse", enableMouse);
    cJSON_AddBoolToObject(root, "fontScale2x", fontScale2x);
    cJSON_AddBoolToObject(root, "enableDebugger", enableDebugger);

    cJSON_AddNumberToObject(root, "displayScaling", (int)displayScaling);

    cJSON_AddNumberToObject(root, "keyLayout", (int)Keyboard::instance()->getKeyLayout());

    cJSON_AddBoolToObject(root, "showMemEdit", showMemEdit);
    cJSON_AddBoolToObject(root, "showCpuState", showCpuState);
    cJSON_AddBoolToObject(root, "showIoRegsWindow", showIoRegsWindow);
    cJSON_AddBoolToObject(root, "showBreakpoints", showBreakpoints);
    cJSON_AddBoolToObject(root, "showAssemblyListing", showAssemblyListing);
    cJSON_AddBoolToObject(root, "showCpuTrace", showCpuTrace);
    cJSON_AddBoolToObject(root, "showWatch", showWatch);
    cJSON_AddBoolToObject(root, "showEspInfo", showEspInfo);
    cJSON_AddBoolToObject(root, "stopOnHalt", stopOnHalt);

    cJSON_AddNumberToObject(root, "memEditMemSelect", memEditMemSelect);

    // Store all NVS U8 items
    {
        cJSON *obj = cJSON_AddObjectToObject(root, "nvs_u8");
        for (auto &item : nvs_u8) {
            cJSON_AddNumberToObject(obj, item.first.c_str(), item.second);
        }
    }

    saveConfigFile("config.json", root);
}
