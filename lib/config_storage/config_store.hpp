#pragma once

#include "node_config.hpp"

namespace gathra {

class ConfigStore {
 public:
  bool load(NodeConfig& config, const char* defaultNodeId);
  bool save(const NodeConfig& config);
  const char* lastError() const { return lastError_; }

 private:
  const char* lastError_ = "not initialized";
};

}  // namespace gathra
