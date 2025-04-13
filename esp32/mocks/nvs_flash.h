#pragma once

#include <stdint.h>

typedef uint32_t nvs_handle_t;
typedef int      esp_err_t;

#define ESP_OK   0
#define ESP_FAIL (-1)

#define ESP_ERROR_CHECK(x) (x)

#define ESP_ERR_NVS_BASE              0x1100                    /*!< Starting number of error codes */
#define ESP_ERR_NVS_NO_FREE_PAGES     (ESP_ERR_NVS_BASE + 0x0d) /*!< NVS partition doesn't contain any empty pages. This may happen if NVS partition was truncated. Erase the whole partition and call nvs_flash_init again. */
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10) /*!< NVS partition contains data in new format and cannot be recognized by this version of code */

typedef enum {
    NVS_READONLY,
    NVS_READWRITE
} nvs_open_mode_t;

esp_err_t nvs_flash_erase();
esp_err_t nvs_flash_init();

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void      nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t c_handle);
esp_err_t nvs_get_u8(nvs_handle_t c_handle, const char *key, uint8_t *out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
