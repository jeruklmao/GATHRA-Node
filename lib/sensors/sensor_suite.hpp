#pragma once

#include "battery_monitor.hpp"
#include "dht22_sensor.hpp"
#include "model.hpp"
#include "node_config.hpp"
#include "srf05_driver.hpp"

namespace gathra {

class SensorSuite {
 public:
  SensorSuite();
  bool begin();
  EnvironmentReading readEnvironment();
  BatteryReading readBattery(const NodeConfig& config);
  SonarBurst readSonarBurst(const NodeConfig& config,
                            const EnvironmentReading& environment);

 private:
  Dht22Sensor dht_;
  Srf05Driver sonar_;
  BatteryMonitor battery_;
  bool sonarReady_ = false;
};

}  // namespace gathra
