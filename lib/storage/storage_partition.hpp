#pragma once

namespace gathra {

// Initializes the dedicated v2 NVS partition. On first installation it may
// occupy bytes that belonged to an obsolete v1 OTA slot, so only the standard
// NVS "not formatted" errors authorize a one-time partition-local erase.
bool initializeV2Storage(bool& formattedFresh);

}  // namespace gathra
