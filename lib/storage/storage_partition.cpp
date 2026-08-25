#include "storage_partition.hpp"

#include "build_config.hpp"

#ifdef ARDUINO
#include <nvs_flash.h>

namespace gathra {

bool initializeV2Storage(bool& formattedFresh) {
  formattedFresh = false;
  esp_err_t result = nvs_flash_init_partition(build::kStoragePartition);
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    if (nvs_flash_erase_partition(build::kStoragePartition) != ESP_OK) {
      return false;
    }
    formattedFresh = true;
    result = nvs_flash_init_partition(build::kStoragePartition);
  }
  return result == ESP_OK;
}

}  // namespace gathra
#endif
