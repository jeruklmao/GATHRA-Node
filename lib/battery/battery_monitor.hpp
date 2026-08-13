#pragma once

#include "model.hpp"
#include "node_config.hpp"

namespace gathra {

class BatteryMonitor {
 public:
  void begin();
  BatteryReading read(const NodeConfig& config);
};

}  // namespace gathra
