#include "nvs_flash.h"
#include "Config.h"

esp_err_t nvs_flash_erase() {
    return ESP_OK;
}

esp_err_t nvs_flash_init() {
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
}

esp_err_t nvs_commit(nvs_handle_t c_handle) {
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t c_handle, const char *key, uint8_t *out_value) {
    auto config = Config::instance();
    auto it     = config->nvs_u8.find(key);
    if (it == config->nvs_u8.end())
        return ESP_FAIL;

    if (out_value)
        *out_value = it->second;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    auto config = Config::instance();
    config->nvs_u8.insert_or_assign(key, value);
    return ESP_OK;
}
