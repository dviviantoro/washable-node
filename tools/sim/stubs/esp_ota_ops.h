#pragma once
struct esp_partition_t { const char *label; };
inline const esp_partition_t *esp_ota_get_running_partition() { static esp_partition_t p = { "app0" }; return &p; }
