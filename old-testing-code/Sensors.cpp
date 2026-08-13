#include <Arduino.h>
#include <DHT.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <math.h>


// ============================================================================
// Configuration
// ============================================================================

namespace config {

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------

constexpr uint32_t kSerialBaudRate = 115200;

// ---------------------------------------------------------------------------
// DHT22 / AM2302
// ---------------------------------------------------------------------------

constexpr uint8_t kDhtPin = 6;
constexpr uint8_t kDhtType = DHT22;
constexpr uint8_t kDhtPullTimeUs = 55;

constexpr uint32_t kDhtPeriodMs = 2500;

// Environmental data older than this is considered stale and will not be
// used for acoustic compensation.
constexpr uint32_t kEnvironmentMaxAgeMs = 10000;

// ---------------------------------------------------------------------------
// HY-SRF05 - two-pin mode
// ---------------------------------------------------------------------------

constexpr gpio_num_t kSonarTriggerPin = GPIO_NUM_7;
constexpr gpio_num_t kSonarEchoPin = GPIO_NUM_10;

constexpr uint32_t kSonarTriggerPulseUs = 10;

constexpr uint32_t kSonarEchoTimeoutUs = 35000;

// Keep at least ~50 ms between ultrasonic ranging commands.
constexpr uint32_t kSonarInterPingDelayMs = 60;

constexpr size_t kSonarSampleCount = 5;
constexpr size_t kSonarMinimumValidSamples = 3;

constexpr uint32_t kSonarPeriodMs = 1000;

// Echo pulse plausibility limits.
constexpr uint32_t kMinimumValidEchoUs = 100;
constexpr uint32_t kMaximumValidEchoUs = 30000;

// ---------------------------------------------------------------------------
// Speed-of-sound model
//
// Engineering approximation:
//
// c = 331.3 + 0.606*T + 0.0124*RH
//
// where:
//   c  = speed of sound [m/s]
//   T  = air temperature [deg C]
//   RH = relative humidity [%RH, 0...100]
// ---------------------------------------------------------------------------

constexpr float kSpeedOfSoundBaseMps = 331.3F;
constexpr float kTemperatureCoefficient = 0.606F;
constexpr float kHumidityCoefficient = 0.0124F;

// ---------------------------------------------------------------------------
// FreeRTOS
// ---------------------------------------------------------------------------

constexpr uint32_t kDhtTaskStackBytes = 4096;
constexpr uint32_t kSonarTaskStackBytes = 6144;

constexpr UBaseType_t kDhtTaskPriority = 2;
constexpr UBaseType_t kSonarTaskPriority = 2;

}  // namespace config


// ============================================================================
// Synchronization
// ============================================================================

// DHT decoding is timing-sensitive and temporarily suppresses interrupts.
// Do not let it overlap an ultrasonic Echo capture.
SemaphoreHandle_t sensorTimingMutex = nullptr;

// Protects environmental state shared between DHT Task and Sonar Task.
SemaphoreHandle_t environmentMutex = nullptr;

// Prevents output from different tasks becoming interleaved.
SemaphoreHandle_t serialMutex = nullptr;


// ============================================================================
// DHT object
// ============================================================================

DHT dht(
    config::kDhtPin,
    config::kDhtType
);


// ============================================================================
// Shared environmental state
// ============================================================================

struct EnvironmentState {
  bool valid = false;

  float temperatureC = NAN;
  float humidityPercent = NAN;

  uint32_t updatedAtMs = 0;
};


struct EnvironmentSnapshot {
  bool valid = false;

  float temperatureC = NAN;
  float humidityPercent = NAN;

  uint32_t ageMs = 0;

  float temperatureOnlySpeedOfSoundMps = NAN;
  float compensatedSpeedOfSoundMps = NAN;
};


EnvironmentState environmentState;


// ============================================================================
// Acoustic calculations
// ============================================================================

float calculateTemperatureOnlySpeedOfSoundMps(
    float temperatureC) {

  return
      config::kSpeedOfSoundBaseMps +
      config::kTemperatureCoefficient * temperatureC;
}


float calculateCompensatedSpeedOfSoundMps(
    float temperatureC,
    float humidityPercent) {

  return
      config::kSpeedOfSoundBaseMps +
      config::kTemperatureCoefficient * temperatureC +
      config::kHumidityCoefficient * humidityPercent;
}


float echoTimeToDistanceCm(
    uint32_t echoTimeUs,
    float speedOfSoundMps) {

  // Ultrasound performs a round trip:
  //
  // sensor -> target -> sensor
  //
  // d = c * t / 2
  //
  // echoTimeUs is in microseconds.
  // speedOfSoundMps is in metres per second.
  //
  // Converting the resulting distance to centimetres gives:
  //
  // d_cm = echoTimeUs * speedOfSoundMps / 20000

  return
      static_cast<float>(echoTimeUs) *
      speedOfSoundMps /
      20000.0F;
}


float echoTimeToReferenceCm(
    uint32_t echoTimeUs) {

  // Conventional SRF05 approximation.
  return static_cast<float>(echoTimeUs) / 58.0F;
}


// ============================================================================
// Environmental state handling
// ============================================================================

void updateEnvironmentState(
    float temperatureC,
    float humidityPercent) {

  xSemaphoreTake(
      environmentMutex,
      portMAX_DELAY
  );

  environmentState.valid =
      !isnan(temperatureC) &&
      !isnan(humidityPercent) &&
      humidityPercent >= 0.0F &&
      humidityPercent <= 100.0F;

  if (environmentState.valid) {

    environmentState.temperatureC =
        temperatureC;

    environmentState.humidityPercent =
        humidityPercent;

    environmentState.updatedAtMs =
        millis();
  }

  xSemaphoreGive(
      environmentMutex
  );
}


EnvironmentSnapshot getEnvironmentSnapshot() {

  EnvironmentSnapshot snapshot;

  xSemaphoreTake(
      environmentMutex,
      portMAX_DELAY
  );

  const EnvironmentState state =
      environmentState;

  xSemaphoreGive(
      environmentMutex
  );

  if (!state.valid) {
    return snapshot;
  }

  const uint32_t nowMs =
      millis();

  // uint32_t subtraction remains safe across millis() wraparound.
  snapshot.ageMs =
      nowMs - state.updatedAtMs;

  if (snapshot.ageMs >
      config::kEnvironmentMaxAgeMs) {

    return snapshot;
  }

  snapshot.temperatureC =
      state.temperatureC;

  snapshot.humidityPercent =
      state.humidityPercent;

  snapshot.temperatureOnlySpeedOfSoundMps =
      calculateTemperatureOnlySpeedOfSoundMps(
          snapshot.temperatureC
      );

  snapshot.compensatedSpeedOfSoundMps =
      calculateCompensatedSpeedOfSoundMps(
          snapshot.temperatureC,
          snapshot.humidityPercent
      );

  snapshot.valid = true;

  return snapshot;
}


// ============================================================================
// Raw ESP32-C3 SRF05 driver
// ============================================================================

class Srf05RawDriver {
 public:

  enum class Status : uint8_t {
    kOk,
    kNotInitialized,
    kEchoHighBeforeTrigger,
    kNoRisingEdge,
    kNoFallingEdge,
    kPulseTooShort,
    kPulseTooLong
  };


  struct Measurement {

    Status status = Status::kNotInitialized;

    uint32_t triggerEndUs = 0;

    uint32_t echoRiseUs = 0;
    uint32_t echoFallUs = 0;

    uint32_t triggerToEchoUs = 0;
    uint32_t echoPulseUs = 0;

    uint32_t risingEdgeCount = 0;
    uint32_t fallingEdgeCount = 0;


    bool valid() const {
      return status == Status::kOk;
    }


    float referenceDistanceCm() const {

      if (!valid()) {
        return NAN;
      }

      return echoTimeToReferenceCm(
          echoPulseUs
      );
    }


    float temperatureOnlyDistanceCm(
        float speedOfSoundMps) const {

      if (!valid() ||
          isnan(speedOfSoundMps)) {

        return NAN;
      }

      return echoTimeToDistanceCm(
          echoPulseUs,
          speedOfSoundMps
      );
    }


    float compensatedDistanceCm(
        float compensatedSpeedOfSoundMps) const {

      if (!valid() ||
          isnan(compensatedSpeedOfSoundMps)) {

        return NAN;
      }

      return echoTimeToDistanceCm(
          echoPulseUs,
          compensatedSpeedOfSoundMps
      );
    }
  };


  Srf05RawDriver(
      gpio_num_t triggerPin,
      gpio_num_t echoPin)
      : triggerPin_(triggerPin),
        echoPin_(echoPin) {}


  esp_err_t begin(
      TaskHandle_t ownerTask) {

    if (ownerTask == nullptr) {
      return ESP_ERR_INVALID_ARG;
    }

    ownerTask_ = ownerTask;

    // -----------------------------------------------------------------------
    // Trigger GPIO
    // -----------------------------------------------------------------------

    gpio_config_t triggerConfig = {};

    triggerConfig.pin_bit_mask =
        1ULL <<
        static_cast<uint32_t>(
            triggerPin_
        );

    triggerConfig.mode =
        GPIO_MODE_OUTPUT;

    triggerConfig.pull_up_en =
        GPIO_PULLUP_DISABLE;

    triggerConfig.pull_down_en =
        GPIO_PULLDOWN_DISABLE;

    triggerConfig.intr_type =
        GPIO_INTR_DISABLE;

    esp_err_t error =
        gpio_config(
            &triggerConfig
        );

    if (error != ESP_OK) {
      return error;
    }

    error =
        gpio_set_level(
            triggerPin_,
            0
        );

    if (error != ESP_OK) {
      return error;
    }

    // -----------------------------------------------------------------------
    // Echo GPIO
    // -----------------------------------------------------------------------

    gpio_config_t echoConfig = {};

    echoConfig.pin_bit_mask =
        1ULL <<
        static_cast<uint32_t>(
            echoPin_
        );

    echoConfig.mode =
        GPIO_MODE_INPUT;

    echoConfig.pull_up_en =
        GPIO_PULLUP_DISABLE;

    echoConfig.pull_down_en =
        GPIO_PULLDOWN_DISABLE;

    echoConfig.intr_type =
        GPIO_INTR_DISABLE;

    error =
        gpio_config(
            &echoConfig
        );

    if (error != ESP_OK) {
      return error;
    }

    // The Arduino core may already have installed the common GPIO ISR
    // service. ESP_ERR_INVALID_STATE is therefore acceptable here.
    error =
        gpio_install_isr_service(0);

    if (error != ESP_OK &&
        error != ESP_ERR_INVALID_STATE) {

      return error;
    }

    error =
        gpio_isr_handler_add(
            echoPin_,
            &Srf05RawDriver::echoIsrThunk,
            this
        );

    if (error != ESP_OK) {
      return error;
    }

    error =
        gpio_set_intr_type(
            echoPin_,
            GPIO_INTR_ANYEDGE
        );

    if (error != ESP_OK) {
      return error;
    }

    error =
        gpio_intr_enable(
            echoPin_
        );

    if (error != ESP_OK) {
      return error;
    }

    initialized_ = true;

    return ESP_OK;
  }


  Measurement measure(
      uint32_t timeoutUs) {

    Measurement result;

    if (!initialized_ ||
        ownerTask_ == nullptr) {

      result.status =
          Status::kNotInitialized;

      return result;
    }

    // Clear any stale notification from an earlier measurement.
    (void)ulTaskNotifyTake(
        pdTRUE,
        0
    );

    resetCaptureState();

    // The Echo line should be LOW while the SRF05 is idle.
    if (gpio_get_level(echoPin_) != 0) {

      result.status =
          Status::kEchoHighBeforeTrigger;

      return result;
    }

    captureActive_ = true;

    // -----------------------------------------------------------------------
    // Generate Trigger pulse
    // -----------------------------------------------------------------------

    gpio_set_level(
        triggerPin_,
        0
    );

    delayMicroseconds(2);

    gpio_set_level(
        triggerPin_,
        1
    );

    delayMicroseconds(
        config::kSonarTriggerPulseUs
    );

    gpio_set_level(
        triggerPin_,
        0
    );

    triggerEndUs_ =
        static_cast<uint32_t>(
            esp_timer_get_time()
        );

    // -----------------------------------------------------------------------
    // Wait for Echo completion
    // -----------------------------------------------------------------------

    const uint32_t timeoutMs =
        (timeoutUs + 999U) / 1000U;

    TickType_t timeoutTicks =
        pdMS_TO_TICKS(timeoutMs);

    if (timeoutTicks == 0) {
      timeoutTicks = 1;
    }

    const uint32_t notificationCount =
        ulTaskNotifyTake(
            pdTRUE,
            timeoutTicks
        );

    captureActive_ = false;

    // -----------------------------------------------------------------------
    // Copy ISR state into normal task-owned result
    // -----------------------------------------------------------------------

    result.triggerEndUs =
        triggerEndUs_;

    result.echoRiseUs =
        echoRiseUs_;

    result.echoFallUs =
        echoFallUs_;

    result.risingEdgeCount =
        risingEdgeCount_;

    result.fallingEdgeCount =
        fallingEdgeCount_;

    // -----------------------------------------------------------------------
    // Validate captured waveform
    // -----------------------------------------------------------------------

    if (notificationCount == 0) {

      if (!sawRisingEdge_) {

        result.status =
            Status::kNoRisingEdge;

        return result;
      }

      result.status =
          Status::kNoFallingEdge;

      return result;
    }

    if (!sawRisingEdge_) {

      result.status =
          Status::kNoRisingEdge;

      return result;
    }

    if (!sawFallingEdge_) {

      result.status =
          Status::kNoFallingEdge;

      return result;
    }

    result.triggerToEchoUs =
        result.echoRiseUs -
        result.triggerEndUs;

    result.echoPulseUs =
        result.echoFallUs -
        result.echoRiseUs;

    if (result.echoPulseUs <
        config::kMinimumValidEchoUs) {

      result.status =
          Status::kPulseTooShort;

      return result;
    }

    if (result.echoPulseUs >
        config::kMaximumValidEchoUs) {

      result.status =
          Status::kPulseTooLong;

      return result;
    }

    result.status =
        Status::kOk;

    return result;
  }


  static const char* statusToString(
      Status status) {

    switch (status) {

      case Status::kOk:
        return "OK";

      case Status::kNotInitialized:
        return "DRIVER NOT INITIALIZED";

      case Status::kEchoHighBeforeTrigger:
        return "ECHO HIGH before trigger";

      case Status::kNoRisingEdge:
        return "TIMEOUT: no Echo rising edge";

      case Status::kNoFallingEdge:
        return "TIMEOUT: Echo rose but never fell";

      case Status::kPulseTooShort:
        return "INVALID: Echo pulse too short";

      case Status::kPulseTooLong:
        return "INVALID: Echo pulse too long";

      default:
        return "UNKNOWN";
    }
  }


 private:

  static void echoIsrThunk(
      void* argument) {

    auto* driver =
        static_cast<Srf05RawDriver*>(
            argument
        );

    driver->handleEchoInterrupt();
  }


  void handleEchoInterrupt() {

    if (!captureActive_) {
      return;
    }

    const uint32_t nowUs =
        static_cast<uint32_t>(
            esp_timer_get_time()
        );

    const int level =
        gpio_get_level(
            echoPin_
        );

    // -----------------------------------------------------------------------
    // Rising edge
    // -----------------------------------------------------------------------

    if (level != 0) {

      ++risingEdgeCount_;

      if (!sawRisingEdge_) {

        echoRiseUs_ = nowUs;
        sawRisingEdge_ = true;
      }

      return;
    }

    // -----------------------------------------------------------------------
    // Falling edge
    // -----------------------------------------------------------------------

    ++fallingEdgeCount_;

    if (!sawRisingEdge_) {
      return;
    }

    if (sawFallingEdge_) {
      return;
    }

    echoFallUs_ = nowUs;
    sawFallingEdge_ = true;

    captureActive_ = false;

    BaseType_t higherPriorityTaskWoken =
        pdFALSE;

    vTaskNotifyGiveFromISR(
        ownerTask_,
        &higherPriorityTaskWoken
    );

    if (higherPriorityTaskWoken ==
        pdTRUE) {

      portYIELD_FROM_ISR();
    }
  }


  void resetCaptureState() {

    captureActive_ = false;

    sawRisingEdge_ = false;
    sawFallingEdge_ = false;

    triggerEndUs_ = 0;

    echoRiseUs_ = 0;
    echoFallUs_ = 0;

    risingEdgeCount_ = 0;
    fallingEdgeCount_ = 0;
  }


  const gpio_num_t triggerPin_;
  const gpio_num_t echoPin_;

  TaskHandle_t ownerTask_ = nullptr;

  bool initialized_ = false;

  volatile bool captureActive_ = false;

  volatile bool sawRisingEdge_ = false;
  volatile bool sawFallingEdge_ = false;

  volatile uint32_t triggerEndUs_ = 0;

  volatile uint32_t echoRiseUs_ = 0;
  volatile uint32_t echoFallUs_ = 0;

  volatile uint32_t risingEdgeCount_ = 0;
  volatile uint32_t fallingEdgeCount_ = 0;
};


Srf05RawDriver sonar(
    config::kSonarTriggerPin,
    config::kSonarEchoPin
);


// ============================================================================
// DHT result
// ============================================================================

struct DhtTestResult {

  bool readOk = false;

  float humidityPercent = NAN;

  float temperatureC = NAN;
  float temperatureF = NAN;

  float heatIndexC = NAN;
  float heatIndexF = NAN;

  float convertedF = NAN;
  float convertedC = NAN;

  float temperatureOnlySpeedOfSoundMps = NAN;
  float compensatedSpeedOfSoundMps = NAN;
};


// ============================================================================
// Sonar batch result
// ============================================================================

struct SonarBatchResult {

  Srf05RawDriver::Measurement
      samples[config::kSonarSampleCount];

  EnvironmentSnapshot environment;

  size_t validSampleCount = 0;

  bool medianValid = false;

  uint32_t medianEchoUs = 0;

  float medianReferenceDistanceCm = NAN;

  float medianTemperatureOnlyDistanceCm = NAN;

  float medianCompensatedDistanceCm = NAN;
};


// ============================================================================
// Utility
// ============================================================================

[[noreturn]] void fatalError(
    const char* message) {

  Serial.println();

  Serial.println(
      "=================================================="
  );

  Serial.println(
      "FATAL ERROR"
  );

  Serial.println(
      message
  );

  Serial.println(
      "System halted."
  );

  Serial.println(
      "=================================================="
  );

  for (;;) {

    vTaskDelay(
        pdMS_TO_TICKS(1000)
    );
  }
}


[[noreturn]] void fatalEspError(
    const char* operation,
    esp_err_t error) {

  Serial.println();

  Serial.println(
      "=================================================="
  );

  Serial.println(
      "FATAL ESP-IDF ERROR"
  );

  Serial.printf(
      "Operation : %s\n",
      operation
  );

  Serial.printf(
      "Error     : %s (0x%X)\n",
      esp_err_to_name(error),
      static_cast<unsigned int>(
          error
      )
  );

  Serial.println(
      "System halted."
  );

  Serial.println(
      "=================================================="
  );

  for (;;) {

    vTaskDelay(
        pdMS_TO_TICKS(1000)
    );
  }
}


void sortAscending(
    uint32_t* values,
    size_t count) {

  // Insertion sort is sufficient here because at most five values
  // are sorted per acquisition batch.

  for (size_t i = 1;
       i < count;
       ++i) {

    const uint32_t current =
        values[i];

    size_t j = i;

    while (
        j > 0 &&
        values[j - 1] > current) {

      values[j] =
          values[j - 1];

      --j;
    }

    values[j] =
        current;
  }
}


// ============================================================================
// DHT acquisition
// ============================================================================

DhtTestResult runDhtTest() {

  DhtTestResult result;

  result.readOk =
      dht.read(true);

  if (!result.readOk) {
    return result;
  }

  result.humidityPercent =
      dht.readHumidity(false);

  result.temperatureC =
      dht.readTemperature(
          false,
          false
      );

  result.temperatureF =
      dht.readTemperature(
          true,
          false
      );

  if (
      isnan(result.humidityPercent) ||
      isnan(result.temperatureC) ||
      isnan(result.temperatureF) ||
      result.humidityPercent < 0.0F ||
      result.humidityPercent > 100.0F) {

    result.readOk = false;

    return result;
  }

  result.convertedF =
      dht.convertCtoF(
          result.temperatureC
      );

  result.convertedC =
      dht.convertFtoC(
          result.convertedF
      );

  result.heatIndexC =
      dht.computeHeatIndex(
          result.temperatureC,
          result.humidityPercent,
          false
      );

  result.heatIndexF =
      dht.computeHeatIndex(
          result.temperatureF,
          result.humidityPercent,
          true
      );

  result.temperatureOnlySpeedOfSoundMps =
      calculateTemperatureOnlySpeedOfSoundMps(
          result.temperatureC
      );

  result.compensatedSpeedOfSoundMps =
      calculateCompensatedSpeedOfSoundMps(
          result.temperatureC,
          result.humidityPercent
      );

  return result;
}


// ============================================================================
// DHT output
// ============================================================================

void printDhtResult(
    const DhtTestResult& result) {

  xSemaphoreTake(
      serialMutex,
      portMAX_DELAY
  );

  Serial.println();

  Serial.println(
      "=================================================="
  );

  Serial.println(
      "[DHT22 / AM2302 TEST]"
  );

  Serial.println(
      "=================================================="
  );

  Serial.printf(
      "GPIO              : %u\n",
      config::kDhtPin
  );

  Serial.println(
      "Library           : Adafruit DHT"
  );

  Serial.printf(
      "read(force=true)  : %s\n",
      result.readOk
          ? "PASS"
          : "FAIL"
  );

  if (!result.readOk) {

    Serial.println(
        "Sensor read failed."
    );

    xSemaphoreGive(
        serialMutex
    );

    return;
  }

  Serial.println();

  Serial.printf(
      "Humidity          : %.1f %%RH\n",
      result.humidityPercent
  );

  Serial.printf(
      "Temperature C     : %.1f C\n",
      result.temperatureC
  );

  Serial.printf(
      "Temperature F     : %.1f F\n",
      result.temperatureF
  );

  Serial.println();

  Serial.println(
      "[SPEED OF SOUND]"
  );

  Serial.printf(
      "Temperature only  : %.3f m/s\n",
      result.temperatureOnlySpeedOfSoundMps
  );

  Serial.printf(
      "Temp + humidity   : %.3f m/s\n",
      result.compensatedSpeedOfSoundMps
  );

  Serial.printf(
      "Humidity effect   : %+.3f m/s\n",
      result.compensatedSpeedOfSoundMps -
          result.temperatureOnlySpeedOfSoundMps
  );

  Serial.println();

  Serial.println(
      "[Conversion API]"
  );

  Serial.printf(
      "convertCtoF()     : %.2f F\n",
      result.convertedF
  );

  Serial.printf(
      "convertFtoC()     : %.2f C\n",
      result.convertedC
  );

  Serial.println();

  Serial.println(
      "[Heat Index API]"
  );

  Serial.printf(
      "Heat index C      : %.2f C\n",
      result.heatIndexC
  );

  Serial.printf(
      "Heat index F      : %.2f F\n",
      result.heatIndexF
  );

  Serial.println(
      "=================================================="
  );

  xSemaphoreGive(
      serialMutex
  );
}


// ============================================================================
// Sonar acquisition
// ============================================================================

SonarBatchResult runSonarBatch() {

  SonarBatchResult result;

  // Take one immutable atmospheric snapshot for all five pings.
  //
  // Temperature and humidity therefore cannot change halfway through
  // this ranging batch.
  result.environment =
      getEnvironmentSnapshot();

  uint32_t validEchoValues[
      config::kSonarSampleCount
  ] = {};

  for (
      size_t i = 0;
      i < config::kSonarSampleCount;
      ++i) {

    xSemaphoreTake(
        sensorTimingMutex,
        portMAX_DELAY
    );

    result.samples[i] =
        sonar.measure(
            config::kSonarEchoTimeoutUs
        );

    xSemaphoreGive(
        sensorTimingMutex
    );

    if (result.samples[i].valid()) {

      validEchoValues[
          result.validSampleCount
      ] =
          result.samples[i].echoPulseUs;

      ++result.validSampleCount;
    }

    if (
        i + 1 <
        config::kSonarSampleCount) {

      vTaskDelay(
          pdMS_TO_TICKS(
              config::kSonarInterPingDelayMs
          )
      );
    }
  }

  if (
      result.validSampleCount <
      config::kSonarMinimumValidSamples) {

    return result;
  }

  sortAscending(
      validEchoValues,
      result.validSampleCount
  );

  const size_t middle =
      result.validSampleCount / 2;

  if (
      (result.validSampleCount % 2U)
      != 0U) {

    result.medianEchoUs =
        validEchoValues[
            middle
        ];

  } else {

    result.medianEchoUs =
        validEchoValues[
            middle - 1
        ] +
        (
            validEchoValues[
                middle
            ] -
            validEchoValues[
                middle - 1
            ]
        ) /
        2U;
  }

  result.medianReferenceDistanceCm =
      echoTimeToReferenceCm(
          result.medianEchoUs
      );

  if (result.environment.valid) {

    result.medianTemperatureOnlyDistanceCm =
        echoTimeToDistanceCm(
            result.medianEchoUs,
            result.environment
                .temperatureOnlySpeedOfSoundMps
        );

    result.medianCompensatedDistanceCm =
        echoTimeToDistanceCm(
            result.medianEchoUs,
            result.environment
                .compensatedSpeedOfSoundMps
        );
  }

  result.medianValid = true;

  return result;
}


// ============================================================================
// Sonar output
// ============================================================================

void printSonarResult(
    const SonarBatchResult& result) {

  xSemaphoreTake(
      serialMutex,
      portMAX_DELAY
  );

  Serial.println();

  Serial.println(
      "=================================================="
  );

  Serial.println(
      "[HY-SRF05 RAW ESP32-C3 DRIVER]"
  );

  Serial.println(
      "=================================================="
  );

  Serial.println(
      "Mode              : TWO PIN"
  );

  Serial.printf(
      "Trigger GPIO      : %d\n",
      static_cast<int>(
          config::kSonarTriggerPin
      )
  );

  Serial.printf(
      "Echo GPIO         : %d\n",
      static_cast<int>(
          config::kSonarEchoPin
      )
  );

  Serial.println(
      "Driver            : Native ESP-IDF GPIO"
  );

  Serial.println(
      "Capture           : ANYEDGE ISR"
  );

  Serial.println(
      "Wait mechanism    : FreeRTOS notification"
  );

  Serial.println();

  // -------------------------------------------------------------------------
  // Environmental compensation
  // -------------------------------------------------------------------------

  Serial.println(
      "[ACOUSTIC COMPENSATION]"
  );

  if (result.environment.valid) {

    Serial.printf(
        "Temperature       : %.2f C\n",
        result.environment.temperatureC
    );

    Serial.printf(
        "Humidity          : %.2f %%RH\n",
        result.environment.humidityPercent
    );

    Serial.printf(
        "DHT data age      : %" PRIu32 " ms\n",
        result.environment.ageMs
    );

    Serial.printf(
        "Temp-only speed   : %.3f m/s\n",
        result.environment
            .temperatureOnlySpeedOfSoundMps
    );

    Serial.printf(
        "T + RH speed      : %.3f m/s\n",
        result.environment
            .compensatedSpeedOfSoundMps
    );

    Serial.printf(
        "Humidity effect   : %+.3f m/s\n",
        result.environment
                .compensatedSpeedOfSoundMps -
            result.environment
                .temperatureOnlySpeedOfSoundMps
    );

    Serial.println(
        "Compensation      : TEMPERATURE + HUMIDITY"
    );

  } else {

    Serial.println(
        "Environment       : unavailable/stale"
    );

    Serial.println(
        "Compensation      : INACTIVE"
    );
  }

  Serial.println();

  // -------------------------------------------------------------------------
  // Individual pings
  // -------------------------------------------------------------------------

  for (
      size_t i = 0;
      i < config::kSonarSampleCount;
      ++i) {

    const auto& sample =
        result.samples[i];

    Serial.printf(
        "[PING %u]\n",
        static_cast<unsigned int>(
            i + 1
        )
    );

    Serial.printf(
        "Status            : %s\n",
        Srf05RawDriver::statusToString(
            sample.status
        )
    );

    Serial.printf(
        "Rising IRQ count  : %" PRIu32 "\n",
        sample.risingEdgeCount
    );

    Serial.printf(
        "Falling IRQ count : %" PRIu32 "\n",
        sample.fallingEdgeCount
    );

    if (sample.valid()) {

      Serial.printf(
          "Trigger -> Echo   : %" PRIu32 " us\n",
          sample.triggerToEchoUs
      );

      Serial.printf(
          "Echo pulse        : %" PRIu32 " us\n",
          sample.echoPulseUs
      );

      Serial.printf(
          "Reference /58     : %.2f cm\n",
          sample.referenceDistanceCm()
      );

      if (result.environment.valid) {

        Serial.printf(
            "Temp only        : %.2f cm\n",
            sample.temperatureOnlyDistanceCm(
                result.environment
                    .temperatureOnlySpeedOfSoundMps
            )
        );

        Serial.printf(
            "Temp + humidity  : %.2f cm\n",
            sample.compensatedDistanceCm(
                result.environment
                    .compensatedSpeedOfSoundMps
            )
        );
      }
    }

    Serial.println();
  }

  // -------------------------------------------------------------------------
  // Filtered result
  // -------------------------------------------------------------------------

  Serial.println(
      "[FILTER RESULT]"
  );

  Serial.printf(
      "Valid samples     : %u / %u\n",
      static_cast<unsigned int>(
          result.validSampleCount
      ),
      static_cast<unsigned int>(
          config::kSonarSampleCount
      )
  );

  if (!result.medianValid) {

    Serial.println(
        "Median            : INVALID"
    );

    Serial.println(
        "Result            : NOT ENOUGH VALID SAMPLES"
    );

    Serial.println(
        "=================================================="
    );

    xSemaphoreGive(
        serialMutex
    );

    return;
  }

  Serial.printf(
      "Median Echo       : %" PRIu32 " us\n",
      result.medianEchoUs
  );

  Serial.printf(
      "Reference /58     : %.2f cm\n",
      result.medianReferenceDistanceCm
  );

  if (result.environment.valid) {

    Serial.printf(
        "Temp only         : %.2f cm\n",
        result.medianTemperatureOnlyDistanceCm
    );

    Serial.printf(
        "Temp + humidity   : %.2f cm\n",
        result.medianCompensatedDistanceCm
    );

    const float temperatureCorrectionCm =
        result.medianTemperatureOnlyDistanceCm -
        result.medianReferenceDistanceCm;

    const float humidityCorrectionCm =
        result.medianCompensatedDistanceCm -
        result.medianTemperatureOnlyDistanceCm;

    const float totalCorrectionCm =
        result.medianCompensatedDistanceCm -
        result.medianReferenceDistanceCm;

    Serial.println();

    Serial.printf(
        "Temp correction   : %+.3f cm\n",
        temperatureCorrectionCm
    );

    Serial.printf(
        "RH correction     : %+.3f cm\n",
        humidityCorrectionCm
    );

    Serial.printf(
        "Total correction  : %+.3f cm\n",
        totalCorrectionCm
    );

    Serial.println();

    Serial.println(
        "Primary distance  : TEMP + HUMIDITY COMPENSATED"
    );

  } else {

    Serial.println(
        "Compensated       : UNAVAILABLE"
    );

    Serial.println(
        "Primary distance  : REFERENCE ONLY"
    );
  }

  Serial.println(
      "Result            : PASS"
  );

  Serial.println(
      "=================================================="
  );

  xSemaphoreGive(
      serialMutex
  );
}


// ============================================================================
// FreeRTOS DHT task
// ============================================================================

void dhtTask(
    void* parameter) {

  (void)parameter;

  TickType_t lastWakeTime =
      xTaskGetTickCount();

  const TickType_t period =
      pdMS_TO_TICKS(
          config::kDhtPeriodMs
      );

  for (;;) {

    xSemaphoreTake(
        sensorTimingMutex,
        portMAX_DELAY
    );

    const DhtTestResult result =
        runDhtTest();

    xSemaphoreGive(
        sensorTimingMutex
    );

    if (result.readOk) {

      updateEnvironmentState(
          result.temperatureC,
          result.humidityPercent
      );
    }

    printDhtResult(
        result
    );

    (void)xTaskDelayUntil(
        &lastWakeTime,
        period
    );
  }
}


// ============================================================================
// FreeRTOS sonar task
// ============================================================================

void sonarTask(
    void* parameter) {

  (void)parameter;

  const esp_err_t beginResult =
      sonar.begin(
          xTaskGetCurrentTaskHandle()
      );

  if (beginResult != ESP_OK) {

    fatalEspError(
        "Srf05RawDriver::begin()",
        beginResult
    );
  }

  TickType_t lastWakeTime =
      xTaskGetTickCount();

  const TickType_t period =
      pdMS_TO_TICKS(
          config::kSonarPeriodMs
      );

  for (;;) {

    const SonarBatchResult result =
        runSonarBatch();

    printSonarResult(
        result
    );

    (void)xTaskDelayUntil(
        &lastWakeTime,
        period
    );
  }
}


// ============================================================================
// Arduino setup
// ============================================================================

void setup() {

  Serial.begin(
      config::kSerialBaudRate
  );

  vTaskDelay(
      pdMS_TO_TICKS(500)
  );

  Serial.println();

  Serial.println(
      "=================================================="
  );

  Serial.println(
      "ESP32-C3 SENSOR TEST"
  );

  Serial.println(
      "=================================================="
  );

  Serial.printf(
      "DHT22             : GPIO %u\n",
      config::kDhtPin
  );

  Serial.printf(
      "SRF05 Trigger     : GPIO %d\n",
      static_cast<int>(
          config::kSonarTriggerPin
      )
  );

  Serial.printf(
      "SRF05 Echo        : GPIO %d\n",
      static_cast<int>(
          config::kSonarEchoPin
      )
  );

  Serial.println(
      "SRF05 mode        : TWO PIN"
  );

  Serial.println(
      "Ultrasonic lib    : NONE"
  );

  Serial.println(
      "Compensation      : TEMPERATURE + HUMIDITY"
  );

  Serial.println(
      "Scheduler         : FreeRTOS"
  );

  Serial.println(
      "=================================================="
  );

  // -------------------------------------------------------------------------
  // Synchronization objects
  // -------------------------------------------------------------------------

  sensorTimingMutex =
      xSemaphoreCreateMutex();

  environmentMutex =
      xSemaphoreCreateMutex();

  serialMutex =
      xSemaphoreCreateMutex();

  if (
      sensorTimingMutex == nullptr ||
      environmentMutex == nullptr ||
      serialMutex == nullptr) {

    fatalError(
        "Failed to create FreeRTOS mutex."
    );
  }

  // -------------------------------------------------------------------------
  // DHT initialization
  // -------------------------------------------------------------------------

  dht.begin(
      config::kDhtPullTimeUs
  );

  // -------------------------------------------------------------------------
  // Create DHT task
  // -------------------------------------------------------------------------

  BaseType_t taskResult =
      xTaskCreate(
          dhtTask,
          "DHT22",
          config::kDhtTaskStackBytes,
          nullptr,
          config::kDhtTaskPriority,
          nullptr
      );

  if (taskResult != pdPASS) {

    fatalError(
        "Failed to create DHT task."
    );
  }

  // -------------------------------------------------------------------------
  // Create sonar task
  // -------------------------------------------------------------------------

  taskResult =
      xTaskCreate(
          sonarTask,
          "SRF05",
          config::kSonarTaskStackBytes,
          nullptr,
          config::kSonarTaskPriority,
          nullptr
      );

  if (taskResult != pdPASS) {

    fatalError(
        "Failed to create SRF05 task."
    );
  }
}


// ============================================================================
// Arduino loop
// ============================================================================

void loop() {

  // All acquisition and processing is performed by FreeRTOS tasks.
  // Keep Arduino's default loop task blocked.

  vTaskDelay(
      pdMS_TO_TICKS(1000)
  );
}