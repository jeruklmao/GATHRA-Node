#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

/*
  LoRa Range Test

  Serial commands (115200 baud):
    set <sf> <bw> <cr> <pwr> <freq> <sync> <interval>   bulk set all at once
                              e.g.  set 10 125 6 17 433.0 0x12 3000
    set sf <7-12>             e.g.  set sf 10
    set bw <kHz>              e.g.  set bw 125     (valid: 7.8,10.4,15.6,20.8,31.25,41.7,62.5,125,250)
    set cr <5-8>              e.g.  set cr 6       (means 4/5 to 4/8)
    set pwr <2-20>            e.g.  set pwr 17
    set freq <MHz>            e.g.  set freq 433.0
    set sync <0x00-0xFF>      e.g.  set sync 0x12
    set interval <ms>         e.g.  set interval 3000
    show                      print current config
    stop                      pause transmitting
    start                     resume transmitting
*/

#ifndef LORA_ROLE
#define LORA_ROLE LORA_ROLE_RECEIVER
#endif

#define LORA_ROLE_RECEIVER    1
#define LORA_ROLE_TRANSMITTER 2

#if LORA_ROLE != LORA_ROLE_RECEIVER && LORA_ROLE != LORA_ROLE_TRANSMITTER
#error "LORA_ROLE must be LORA_ROLE_RECEIVER or LORA_ROLE_TRANSMITTER"
#endif

// Hardware connections
#define SCK_PIN  4
#define MISO_PIN 5
#define MOSI_PIN 6
#define CS_PIN   7
#define RST_PIN  2
#define DIO0_PIN 1
#define LED_PIN  8

// The ESP32-C3 Super Mini built-in LED is normally active-low.
#define LED_ON()  digitalWrite(LED_PIN, LOW)
#define LED_OFF() digitalWrite(LED_PIN, HIGH)

SX1278 radio = new Module(CS_PIN, DIO0_PIN, RST_PIN, RADIOLIB_NC);

// Settings shared by receiver and transmitter
float p_freq = 433.0;
float p_bw = 125.0;
int p_sf = 10;
int p_cr = 6;
int p_pwr = 17;
uint8_t p_sync = 0x12;

bool validBandwidth(float value) {
  const float valid[] = {7.8, 10.4, 15.6, 20.8, 31.25,
                         41.7, 62.5, 125.0, 250.0};
  for (float bandwidth : valid) {
    if (fabsf(value - bandwidth) < 0.1f) return true;
  }
  return false;
}

long parseNumber(const String &value) {
  return value.startsWith("0x") ? strtol(value.c_str(), nullptr, 16)
                                : value.toInt();
}

bool firstTokenIsNumeric(const String &token) {
  if (token.isEmpty()) return false;
  for (size_t i = 0; i < token.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(token[i]))) return false;
  }
  return true;
}

int splitTokens(const String &text, String tokens[], int capacity) {
  int count = 0;
  int start = 0;
  for (size_t i = 0; i <= text.length() && count < capacity; ++i) {
    if (i == text.length() || text[i] == ' ') {
      String token = text.substring(start, i);
      token.trim();
      if (!token.isEmpty()) tokens[count++] = token;
      start = i + 1;
    }
  }
  return count;
}

void printCommonConfig() {
  Serial.println(F("------------------------------"));
  Serial.println(F(" Current LoRa Configuration"));
  Serial.println(F("------------------------------"));
  Serial.print(F("  Frequency : ")); Serial.print(p_freq); Serial.println(F(" MHz"));
  Serial.print(F("  Bandwidth : ")); Serial.print(p_bw); Serial.println(F(" kHz"));
  Serial.print(F("  SF        : ")); Serial.println(p_sf);
  Serial.print(F("  CR        : 4/")); Serial.println(p_cr);
  Serial.print(F("  TX Power  : ")); Serial.print(p_pwr); Serial.println(F(" dBm"));
  Serial.print(F("  Sync Word : 0x"));
  if (p_sync < 0x10) Serial.print('0');
  Serial.println(p_sync, HEX);
}

bool validateCommon(int sf, float bw, int cr, int power, float frequency,
                    long sync, const __FlashStringHelper *tag) {
  if (sf < 7 || sf > 12) {
    Serial.print(tag); Serial.println(F(" SF must be 7-12")); return false;
  }
  if (!validBandwidth(bw)) {
    Serial.print(tag); Serial.println(F(" Invalid BW")); return false;
  }
  if (cr < 5 || cr > 8) {
    Serial.print(tag); Serial.println(F(" CR must be 5-8")); return false;
  }
  if (power < 2 || power > 20) {
    Serial.print(tag); Serial.println(F(" Power must be 2-20 dBm")); return false;
  }
  if (frequency < 410.0 || frequency > 525.0) {
    Serial.print(tag); Serial.println(F(" Freq must be 410-525 MHz")); return false;
  }
  if (sync < 0 || sync > 0xFF) {
    Serial.print(tag); Serial.println(F(" Sync must be 0x00-0xFF")); return false;
  }
  return true;
}

#if LORA_ROLE == LORA_ROLE_RECEIVER

volatile bool receivedFlag = false;
uint32_t totalReceived = 0;
uint32_t totalCRCErrors = 0;
uint32_t lostPackets = 0;
int32_t lastSeq = -1;

void IRAM_ATTR radioInterrupt() { receivedFlag = true; }

void applyConfig() {
  radio.standby();
  int state = radio.begin(p_freq, p_bw, p_sf, p_cr, p_sync, p_pwr);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("[GW] Config failed, code ")); Serial.println(state);
    return;
  }
  radio.setPacketReceivedAction(radioInterrupt);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("[GW] startReceive failed, code ")); Serial.println(state);
    return;
  }
  Serial.println(F("[GW] Config applied. Listening..."));
}

void printConfig() {
  printCommonConfig();
  Serial.println(F("------------------------------"));
}

void printStats() {
  Serial.println(F("------------------------------"));
  Serial.println(F(" Packet Statistics"));
  Serial.println(F("------------------------------"));
  Serial.print(F("  Received  : ")); Serial.println(totalReceived);
  Serial.print(F("  CRC Errors: ")); Serial.println(totalCRCErrors);
  Serial.print(F("  Lost (gap): ")); Serial.println(lostPackets);
  const uint32_t attempted = totalReceived + lostPackets + totalCRCErrors;
  if (attempted) {
    Serial.print(F("  Success % : "));
    Serial.print(100.0f * totalReceived / attempted, 1);
    Serial.println('%');
  }
  Serial.println(F("------------------------------"));
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toLowerCase();

  if (line == "show") { printConfig(); return; }
  if (line == "stats") { printStats(); return; }
  if (line == "reset") {
    totalReceived = totalCRCErrors = lostPackets = 0;
    lastSeq = -1;
    Serial.println(F("[GW] Stats reset."));
    return;
  }
  if (!line.startsWith("set ")) {
    Serial.println(F("[GW] Commands: set sf/bw/cr/pwr/freq/sync, show, stats, reset"));
    return;
  }

  String rest = line.substring(4);
  int separator = rest.indexOf(' ');
  if (separator < 0) {
    Serial.println(F("[GW] Usage: set <param> <value> or set <sf> <bw> <cr> <pwr> <freq> <sync>"));
    return;
  }
  String param = rest.substring(0, separator);
  String value = rest.substring(separator + 1);
  value.trim();

  if (firstTokenIsNumeric(param)) {
    String tokens[6];
    if (splitTokens(rest, tokens, 6) != 6) {
      Serial.println(F("[GW] Bulk set needs 6 values."));
      return;
    }
    int sf = tokens[0].toInt();
    float bw = tokens[1].toFloat();
    int cr = tokens[2].toInt();
    int power = tokens[3].toInt();
    float frequency = tokens[4].toFloat();
    long sync = parseNumber(tokens[5]);
    if (!validateCommon(sf, bw, cr, power, frequency, sync, F("[GW]"))) return;
    p_sf = sf; p_bw = bw; p_cr = cr; p_pwr = power;
    p_freq = frequency; p_sync = static_cast<uint8_t>(sync);
    applyConfig();
    printConfig();
    return;
  }

  bool restartRadio = true;
  if (param == "sf") {
    int v = value.toInt();
    if (v < 7 || v > 12) { Serial.println(F("[GW] SF must be 7-12")); return; }
    p_sf = v;
  } else if (param == "bw") {
    float v = value.toFloat();
    if (!validBandwidth(v)) { Serial.println(F("[GW] Invalid BW")); return; }
    p_bw = v;
  } else if (param == "cr") {
    int v = value.toInt();
    if (v < 5 || v > 8) { Serial.println(F("[GW] CR must be 5-8")); return; }
    p_cr = v;
  } else if (param == "pwr") {
    int v = value.toInt();
    if (v < 2 || v > 20) { Serial.println(F("[GW] Power must be 2-20 dBm")); return; }
    p_pwr = v;
  } else if (param == "freq") {
    float v = value.toFloat();
    if (v < 410.0 || v > 525.0) { Serial.println(F("[GW] Freq must be 410-525 MHz")); return; }
    p_freq = v;
  } else if (param == "sync") {
    long v = parseNumber(value);
    if (v < 0 || v > 0xFF) { Serial.println(F("[GW] Sync must be 0x00-0xFF")); return; }
    p_sync = static_cast<uint8_t>(v);
  } else {
    Serial.println(F("[GW] Unknown param. Valid: sf bw cr pwr freq sync"));
    restartRadio = false;
  }
  if (restartRadio) {
    Serial.println(F("[GW] Restarting radio with new config..."));
    applyConfig();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n[GW] LoRa Gateway - Range Tester"));
  Serial.println(F("[GW] Type 'show' for config, 'stats' for stats"));
  pinMode(LED_PIN, OUTPUT);
  LED_OFF();
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  applyConfig();
  printConfig();
}

void loop() {
  handleSerial();
  if (!receivedFlag) return;

  receivedFlag = false;
  LED_ON();
  String payload;
  int state = radio.readData(payload);
  if (state == RADIOLIB_ERR_NONE) {
    ++totalReceived;
    int32_t sequence = -1;
    if (payload.startsWith("PKT:")) {
      sequence = payload.substring(4).toInt();
      if (lastSeq >= 0 && sequence > lastSeq + 1)
        lostPackets += sequence - lastSeq - 1;
      lastSeq = sequence;
    }
    Serial.print(sequence); Serial.print(',');
    Serial.print(radio.getRSSI(), 1); Serial.print(',');
    Serial.print(radio.getSNR(), 1); Serial.print(',');
    Serial.print(radio.getFrequencyError(), 0); Serial.print(',');
    Serial.println(payload);
  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    ++totalCRCErrors;
    Serial.println(F("-1,,,,CRC_ERROR"));
  }
  LED_OFF();
  radio.startReceive();
}

#elif LORA_ROLE == LORA_ROLE_TRANSMITTER

uint32_t p_interval = 3000;
volatile bool txDoneFlag = false;
uint32_t packetCounter = 0;
bool transmitting = true;
bool txInFlight = false;
unsigned long lastTxTime = 0;

void IRAM_ATTR radioInterrupt() { txDoneFlag = true; }

void applyConfig() {
  radio.standby();
  int state = radio.begin(p_freq, p_bw, p_sf, p_cr, p_sync, p_pwr);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("[TX] Config failed, code ")); Serial.println(state);
    return;
  }
  radio.setPacketSentAction(radioInterrupt);
  txInFlight = false;
  Serial.println(F("[TX] Config applied."));
}

void printConfig() {
  printCommonConfig();
  Serial.print(F("  Interval  : ")); Serial.print(p_interval); Serial.println(F(" ms"));
  Serial.print(F("  Status    : "));
  Serial.println(transmitting ? F("TRANSMITTING") : F("STOPPED"));
  Serial.println(F("------------------------------"));
}

void sendPacket() {
  String packet = "PKT:" + String(packetCounter);
  int state = radio.startTransmit(packet);
  if (state == RADIOLIB_ERR_NONE) {
    ++packetCounter;
    txInFlight = true;
    LED_ON();
  } else {
    Serial.print(F("[TX] startTransmit failed, code ")); Serial.println(state);
    txInFlight = false;
    LED_OFF();
    lastTxTime = millis();
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toLowerCase();

  if (line == "show") { printConfig(); return; }
  if (line == "stop") {
    transmitting = false;
    radio.standby();
    txInFlight = false;
    LED_OFF();
    Serial.println(F("[TX] Stopped."));
    return;
  }
  if (line == "start") {
    transmitting = true;
    lastTxTime = millis() - p_interval;
    Serial.println(F("[TX] Started."));
    return;
  }
  if (!line.startsWith("set ")) {
    Serial.println(F("[TX] Commands: set sf/bw/cr/pwr/freq/sync/interval, show, stop, start"));
    return;
  }

  String rest = line.substring(4);
  int separator = rest.indexOf(' ');
  if (separator < 0) {
    Serial.println(F("[TX] Usage: set <param> <value> or set <sf> <bw> <cr> <pwr> <freq> <sync> <interval>"));
    return;
  }
  String param = rest.substring(0, separator);
  String value = rest.substring(separator + 1);
  value.trim();

  if (firstTokenIsNumeric(param)) {
    String tokens[7];
    if (splitTokens(rest, tokens, 7) != 7) {
      Serial.println(F("[TX] Bulk set needs 7 values."));
      return;
    }
    int sf = tokens[0].toInt();
    float bw = tokens[1].toFloat();
    int cr = tokens[2].toInt();
    int power = tokens[3].toInt();
    float frequency = tokens[4].toFloat();
    long sync = parseNumber(tokens[5]);
    long interval = tokens[6].toInt();
    if (!validateCommon(sf, bw, cr, power, frequency, sync, F("[TX]"))) return;
    if (interval < 100 || interval > 60000) {
      Serial.println(F("[TX] Interval must be 100-60000 ms")); return;
    }
    p_sf = sf; p_bw = bw; p_cr = cr; p_pwr = power;
    p_freq = frequency; p_sync = static_cast<uint8_t>(sync);
    p_interval = static_cast<uint32_t>(interval);
    applyConfig();
    printConfig();
    return;
  }

  bool restartRadio = true;
  if (param == "sf") {
    int v = value.toInt();
    if (v < 7 || v > 12) { Serial.println(F("[TX] SF must be 7-12")); return; }
    p_sf = v;
  } else if (param == "bw") {
    float v = value.toFloat();
    if (!validBandwidth(v)) { Serial.println(F("[TX] Invalid BW")); return; }
    p_bw = v;
  } else if (param == "cr") {
    int v = value.toInt();
    if (v < 5 || v > 8) { Serial.println(F("[TX] CR must be 5-8")); return; }
    p_cr = v;
  } else if (param == "pwr") {
    int v = value.toInt();
    if (v < 2 || v > 20) { Serial.println(F("[TX] Power must be 2-20 dBm")); return; }
    p_pwr = v;
  } else if (param == "freq") {
    float v = value.toFloat();
    if (v < 410.0 || v > 525.0) { Serial.println(F("[TX] Freq must be 410-525 MHz")); return; }
    p_freq = v;
  } else if (param == "sync") {
    long v = parseNumber(value);
    if (v < 0 || v > 0xFF) { Serial.println(F("[TX] Sync must be 0x00-0xFF")); return; }
    p_sync = static_cast<uint8_t>(v);
  } else if (param == "interval") {
    long v = value.toInt();
    if (v < 100 || v > 60000) {
      Serial.println(F("[TX] Interval must be 100-60000 ms")); return;
    }
    p_interval = static_cast<uint32_t>(v);
    restartRadio = false;
  } else {
    Serial.println(F("[TX] Unknown param. Valid: sf bw cr pwr freq sync interval"));
    restartRadio = false;
  }
  if (restartRadio) {
    Serial.println(F("[TX] Restarting radio with new config..."));
    applyConfig();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n[TX] LoRa Transmitter - Range Tester"));
  Serial.println(F("[TX] Type 'show' for config, 'stop'/'start' to pause"));
  pinMode(LED_PIN, OUTPUT);
  LED_OFF();
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  applyConfig();
  printConfig();
  lastTxTime = millis() - p_interval;
}

void loop() {
  handleSerial();

  if (txDoneFlag) {
    txDoneFlag = false;
    txInFlight = false;
    LED_OFF();
    int state = radio.finishTransmit();
    if (state == RADIOLIB_ERR_NONE) {
      Serial.print(F("[TX] Sent PKT:")); Serial.println(packetCounter - 1);
    } else {
      Serial.print(F("[TX] Send failed, code ")); Serial.println(state);
    }
    lastTxTime = millis();
  }

  if (transmitting && !txInFlight &&
      millis() - lastTxTime >= p_interval) {
    sendPacket();
  }
}

#endif