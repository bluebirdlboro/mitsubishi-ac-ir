#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <IRac.h>
#include <ir_MitsubishiHeavy.h>

// ===================== User configuration =====================
#include "secrets.h"

const char* DEVICE_ID     = "mitsubishi_ac_01";
const char* DEVICE_NAME   = "Mitsubishi AC IR";
const char* OTA_HOSTNAME  = "mitsubishi-ac-ir";
const char* TOPIC_PREFIX  = "mitsubishi_ac";  // base prefix for command/state topics

const uint16_t IR_LED_PIN = 4;
// Adjust to match your board: the GPIO labelled IR RX (commonly 14 or 15).
const uint16_t IR_RECV_PIN = 14;
const uint16_t WIFI_STATUS_LED = 2;

// ===================== IR receive config =====================
const uint16_t kCaptureBufferSize = 1024;
const uint8_t kTimeout = 50;      // ms idle to consider signal complete
const uint16_t kMinUnknownSize = 50;  // suppress short spurious bursts; full AC frame is >130 transitions

// ===================== AC state =====================
bool acPower = false;
stdAc::opmode_t acMode = stdAc::opmode_t::kCool;
uint8_t acTemp = 26;
stdAc::fanspeed_t acFan = stdAc::fanspeed_t::kAuto;

// ===================== MQTT topics =====================
String topicAvailability;
String topicIRReceived;
String topicACCommand;
String topicACState;
String topicScene;
String topicSceneState;

// ===================== Scene tracking =====================
// "" | "power_cool" | "comfort" | "quiet_sleep" | "away_off"
String currentScene = "";

// ===================== Globals =====================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
IRsend irsend(IR_LED_PIN);
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kTimeout, true);
decode_results results;

unsigned long lastIRSendTime = 0;
const uint16_t IR_IGNORE_WINDOW_MS = 500;

unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastWifiCheck = 0;

// ===================== Helpers =====================
void publishACState();
void publishSceneState();
void setLed(bool on) {
  digitalWrite(WIFI_STATUS_LED, on ? HIGH : LOW);
}

// ===================== Custom Mitsubishi Heavy protocol send =====================
// Reverse-engineered 8-byte frame:
//   B0=FF  B1=00  B2=fan  B3=fan_comp  B4=temp|mode  B5=0xFF-B4  B6=2A  B7=D5
//
// Fan B2/B3:   High=FF/00  Medium=BF/40  Low=9F/60
// Mode B4 lo4: Auto=7  Cool=6  Dry=5  Fan=4  Heat=3  (power off OR'd with 0x8)
// Temp B4 hi4: (32 - temp) & 0xF

void sendMitsubishiCustom(bool power, uint8_t mode_code,
                          uint8_t temp, uint8_t fan_b2, uint8_t fan_b3) {
  uint8_t mc = mode_code;
  if (!power) mc |= 0x8;

  uint8_t temp_code = (32 - temp) & 0xF;
  uint8_t b4 = (temp_code << 4) | mc;
  uint8_t b5 = 0xFF - b4;

  uint8_t frame[8] = {0xFF, 0x00, fan_b2, fan_b3, b4, b5, 0x2A, 0xD5};

  Serial.printf("[IR] TX frame: ");
  for (int i = 0; i < 8; i++) Serial.printf("%02X ", frame[i]);
  Serial.println();

  const uint16_t HDR_MARK  = 5950;
  const uint16_t HDR_SPACE = 7475;
  const uint16_t BIT_MARK  = 508;
  const uint16_t ONE_SPACE  = 3454;
  const uint16_t ZERO_SPACE = 1496;
  const uint16_t TRL_SPACE  = 7422;

  uint16_t raw[132];
  int idx = 0;
  raw[idx++] = HDR_MARK;
  raw[idx++] = HDR_SPACE;

  for (int b = 0; b < 8; b++) {
    for (int bit = 0; bit < 8; bit++) {
      raw[idx++] = BIT_MARK;
      raw[idx++] = (frame[b] >> bit) & 1 ? ONE_SPACE : ZERO_SPACE;
    }
  }

  raw[idx++] = BIT_MARK;
  raw[idx++] = TRL_SPACE;

  for (int r = 0; r < 3; r++) {
    irsend.sendRaw(raw, idx, 38);
    delay(50);
  }
  Serial.println("[IR] TX done");

  lastIRSendTime = millis();
}

void sendACCommand(bool power, stdAc::opmode_t mode, uint8_t temp, stdAc::fanspeed_t fan) {
  Serial.printf("[AC] Power=%s Mode=%d Temp=%u Fan=%d\n",
    power ? "ON" : "OFF", (int)mode, temp, (int)fan);

  uint8_t mc;
  switch (mode) {
    case stdAc::opmode_t::kAuto: mc = 0x7; break;
    case stdAc::opmode_t::kCool: mc = 0x6; break;
    case stdAc::opmode_t::kDry:  mc = 0x5; break;
    case stdAc::opmode_t::kFan:  mc = 0x4; break;
    case stdAc::opmode_t::kHeat: mc = 0x3; break;
    default:                     mc = 0x6; break;  // unknown -> Cool
  }

  uint8_t fb2, fb3;
  switch (fan) {
    case stdAc::fanspeed_t::kMax:
    case stdAc::fanspeed_t::kHigh:   fb2 = 0xFF; fb3 = 0x00; break;
    case stdAc::fanspeed_t::kMedium: fb2 = 0xBF; fb3 = 0x40; break;
    default:                          fb2 = 0x9F; fb3 = 0x60; break;  // Low / Auto
  }

  uint8_t t = (uint8_t)constrain((int)temp, 16, 30);

  sendMitsubishiCustom(power, mc, t, fb2, fb3);
  publishACState();
}

void publishAvailability(const char* status) {
  mqttClient.publish(topicAvailability.c_str(), status, true);
}

void publishACState() {
  if (!mqttClient.connected()) return;

  const char* modeStr = "off";
  if (acPower) {
    switch (acMode) {
      case stdAc::opmode_t::kCool: modeStr = "cool"; break;
      case stdAc::opmode_t::kHeat: modeStr = "heat"; break;
      case stdAc::opmode_t::kDry:  modeStr = "dry"; break;
      case stdAc::opmode_t::kFan:  modeStr = "fan_only"; break;
      default:                     modeStr = "auto"; break;
    }
  }

  const char* fanStr = "low";
  switch (acFan) {
    case stdAc::fanspeed_t::kMax:
    case stdAc::fanspeed_t::kHigh:   fanStr = "high"; break;
    case stdAc::fanspeed_t::kMedium: fanStr = "medium"; break;
    case stdAc::fanspeed_t::kLow:    fanStr = "low"; break;
    default:                         fanStr = "low"; break;
  }

  JsonDocument doc;
  doc["mode"] = modeStr;
  doc["temperature"] = acTemp;
  doc["fan_mode"] = fanStr;
  doc["preset_mode"] = "none";
  if (currentScene == "power_cool")       doc["preset_mode"] = "Power Cool";
  else if (currentScene == "comfort")     doc["preset_mode"] = "Comfort";
  else if (currentScene == "quiet_sleep") doc["preset_mode"] = "Quiet Sleep";
  else if (currentScene == "away_off")    doc["preset_mode"] = "Away/Off";

  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(topicACState.c_str(), buf, true);
  Serial.printf("[MQTT] State: %s\n", buf);
}

void publishSceneState() {
  if (!mqttClient.connected()) return;

  const char* displayName = "Off";
  if (currentScene == "power_cool")       displayName = "Power Cool";
  else if (currentScene == "comfort")     displayName = "Comfort";
  else if (currentScene == "quiet_sleep") displayName = "Quiet Sleep";
  else if (currentScene == "away_off")    displayName = "Away/Off";

  mqttClient.publish(topicSceneState.c_str(), displayName, true);
  Serial.printf("[MQTT] Scene: %s\n", displayName);
}

void publishDiscovery() {
  // Republish on every MQTT (re)connect. Discovery is retained on the broker,
  // so this is idempotent for HA, but it survives a broker losing retained state.

  JsonDocument doc;
  String uniqueId = String(DEVICE_ID) + "_climate";
  doc["name"] = "Mitsubishi AC";
  doc["unique_id"] = uniqueId;
  doc["availability_topic"]    = topicAvailability;
  doc["payload_available"]     = "online";
  doc["payload_not_available"] = "offline";

  doc["mode_command_topic"] = topicACCommand + "/mode";
  doc["temperature_command_topic"] = topicACCommand + "/temp";

  doc["mode_state_topic"] = topicACState;
  doc["mode_state_template"] = "{{ value_json.mode }}";
  doc["temperature_state_topic"] = topicACState;
  doc["temperature_state_template"] = "{{ value_json.temperature }}";
  // If you have a separate room temperature sensor publishing over MQTT,
  // set its topic here to display a "current" temperature in HA.
  // doc["current_temperature_topic"] = "your_sensor/temperature";

  doc["fan_mode_command_topic"] = topicACCommand + "/fan";
  doc["fan_mode_state_topic"] = topicACState;
  doc["fan_mode_state_template"] = "{{ value_json.fan_mode }}";

  JsonArray modes = doc["modes"].to<JsonArray>();
  modes.add("off");
  modes.add("cool");
  modes.add("heat");
  modes.add("fan_only");
  modes.add("dry");
  modes.add("auto");

  // Known limitation: the captured 8-byte protocol has no "auto fan" encoding,
  // so we only expose the three speeds the AC actually supports.
  JsonArray fan_modes = doc["fan_modes"].to<JsonArray>();
  fan_modes.add("high");
  fan_modes.add("medium");
  fan_modes.add("low");

  doc["preset_mode_command_topic"] = topicACCommand + "/preset";
  doc["preset_mode_state_topic"]   = topicACState;
  doc["preset_mode_state_template"] = "{{ value_json.preset_mode }}";
  JsonArray presets = doc["preset_modes"].to<JsonArray>();
  presets.add("Power Cool");
  presets.add("Comfort");
  presets.add("Quiet Sleep");
  presets.add("Away/Off");

  doc["min_temp"] = 16;
  doc["max_temp"] = 30;
  doc["temp_step"] = 1;

  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"].to<JsonArray>().add(DEVICE_ID);
  device["name"]         = DEVICE_NAME;
  device["manufacturer"] = "Bluebird";
  device["model"]        = "ESP32 IR Controller";

  char payload[2048];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  Serial.printf("[MQTT] Discovery payload size: %u\n", len);
  if (len >= sizeof(payload) - 1) {
    Serial.println("[MQTT] WARNING: payload may be truncated");
  }
  String discoveryTopic = String("homeassistant/climate/") + DEVICE_ID + "/config";
  bool ok = mqttClient.publish(discoveryTopic.c_str(), payload, true);
  Serial.printf("[MQTT] Climate discovery publish: %s\n", ok ? "OK" : "FAILED");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] RX %s : %s\n", topic, msg.c_str());

  String t = String(topic);

  if (t == topicACCommand + "/mode") {
    if (msg == "off") {
      acPower = false;
      currentScene = "";
      publishSceneState();
    } else {
      acPower = true;
      if      (msg == "cool")     acMode = stdAc::opmode_t::kCool;
      else if (msg == "heat")     acMode = stdAc::opmode_t::kHeat;
      else if (msg == "dry")      acMode = stdAc::opmode_t::kDry;
      else if (msg == "fan_only") acMode = stdAc::opmode_t::kFan;
      else                        acMode = stdAc::opmode_t::kAuto;
    }
    Serial.printf("[AC] Mode cmd: %s\n", msg.c_str());
    sendACCommand(acPower, acMode, acTemp, acFan);
  }
  else if (t == topicACCommand + "/temp") {
    acTemp = (uint8_t)constrain((int)msg.toFloat(), 16, 30);
    Serial.printf("[AC] Temp cmd: %u\n", acTemp);
    sendACCommand(acPower, acMode, acTemp, acFan);
  }
  else if (t == topicACCommand + "/fan") {
    if (msg == "high")        acFan = stdAc::fanspeed_t::kHigh;
    else if (msg == "medium") acFan = stdAc::fanspeed_t::kMedium;
    else                       acFan = stdAc::fanspeed_t::kLow;
    Serial.printf("[AC] Fan cmd: %s\n", msg.c_str());
    sendACCommand(acPower, acMode, acTemp, acFan);
  }
  else if (t == topicACCommand + "/preset" || t == topicScene + "/set") {
    String sceneName = msg;
    sceneName.trim();
    Serial.printf("[Scene] Select cmd: %s\n", sceneName.c_str());

    if (sceneName == "Power Cool") {
      currentScene = "power_cool";
      acPower = true; acMode = stdAc::opmode_t::kCool;
      acFan = stdAc::fanspeed_t::kHigh; acTemp = 16;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else if (sceneName == "Comfort") {
      currentScene = "comfort";
      acPower = true; acMode = stdAc::opmode_t::kCool;
      acFan = stdAc::fanspeed_t::kMedium; acTemp = 26;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else if (sceneName == "Quiet Sleep") {
      currentScene = "quiet_sleep";
      acPower = true; acMode = stdAc::opmode_t::kCool;
      acFan = stdAc::fanspeed_t::kLow; acTemp = 28;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else if (sceneName == "Away/Off") {
      currentScene = "away_off";
      acPower = false;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else {
      currentScene = "";
    }
    publishSceneState();
  }
  else if (t == topicACCommand + "/set_all") {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      Serial.printf("[set_all] JSON parse failed: %s\n", err.c_str());
      return;
    }
    bool changed = false;
    if (!doc["mode"].isNull()) {
      String mode = doc["mode"].as<String>();
      if (mode == "off") {
        acPower = false;
        currentScene = "";
        publishSceneState();
      } else {
        acPower = true;
        if      (mode == "cool")     acMode = stdAc::opmode_t::kCool;
        else if (mode == "heat")     acMode = stdAc::opmode_t::kHeat;
        else if (mode == "dry")      acMode = stdAc::opmode_t::kDry;
        else if (mode == "fan_only") acMode = stdAc::opmode_t::kFan;
        else                         acMode = stdAc::opmode_t::kAuto;
      }
      changed = true;
    }
    if (!doc["temperature"].isNull()) {
      acTemp = (uint8_t)constrain((int)doc["temperature"].as<float>(), 16, 30);
      changed = true;
    }
    if (!doc["fan_speed"].isNull()) {
      String fan = doc["fan_speed"].as<String>();
      if      (fan == "high")   acFan = stdAc::fanspeed_t::kHigh;
      else if (fan == "medium") acFan = stdAc::fanspeed_t::kMedium;
      else                       acFan = stdAc::fanspeed_t::kLow;
      changed = true;
    }
    if (changed) {
      Serial.printf("[set_all] mode=%s temp=%u fan=%d power=%d\n",
        doc["mode"].as<String>().c_str(), acTemp, (int)acFan, (int)acPower);
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
  }
}

// ===================== IR receive handling =====================

void handleIRReceive() {
  if (!irrecv.decode(&results)) return;

  if (millis() - lastIRSendTime < IR_IGNORE_WINDOW_MS) {
    irrecv.resume();
    return;
  }

  Serial.println("========== IR signal received ==========");
  Serial.printf("Protocol: %s\n", typeToString(results.decode_type).c_str());
  Serial.printf("Raw length: %u\n", results.rawlen - 1);
  Serial.println(resultToHumanReadableBasic(&results));

  bool isMitsubishiHeavy152 = results.decode_type == MITSUBISHI_HEAVY_152;
  bool isMitsubishiHeavy88  = results.decode_type == MITSUBISHI_HEAVY_88;

  if (isMitsubishiHeavy152) {
    Serial.println("Detected Mitsubishi Heavy 152-bit protocol");

    IRMitsubishiHeavy152Ac acDecoder(IR_LED_PIN);
    acDecoder.setRaw(results.state);

    bool   power = acDecoder.getPower();
    uint8_t mode = acDecoder.getMode();
    float   temp = acDecoder.getTemp();
    uint8_t fan  = acDecoder.getFan();

    Serial.printf("  Power: %s\n", power ? "ON" : "OFF");
    Serial.printf("  Mode:  %u\n", mode);
    Serial.printf("  Temp:  %.1f C\n", temp);
    Serial.printf("  Fan:   %u\n", fan);

    if (mqttClient.connected()) {
      JsonDocument doc;
      doc["protocol"] = "MITSUBISHI_HEAVY_152";
      doc["power"]    = power;
      doc["mode"]     = mode;
      doc["temp"]     = temp;
      doc["fan"]      = fan;
      char buf[256];
      serializeJson(doc, buf, sizeof(buf));
      mqttClient.publish(topicIRReceived.c_str(), buf);
      Serial.println("[MQTT] Decoded frame published");
    }

  } else if (isMitsubishiHeavy88) {
    Serial.println("Detected Mitsubishi Heavy 88-bit protocol");

    IRMitsubishiHeavy88Ac acDecoder(IR_LED_PIN);
    acDecoder.setRaw(results.state);

    bool   power = acDecoder.getPower();
    uint8_t mode = acDecoder.getMode();
    float   temp = acDecoder.getTemp();
    uint8_t fan  = acDecoder.getFan();

    Serial.printf("  Power: %s\n", power ? "ON" : "OFF");
    Serial.printf("  Mode:  %u\n", mode);
    Serial.printf("  Temp:  %.1f C\n", temp);
    Serial.printf("  Fan:   %u\n", fan);

    if (mqttClient.connected()) {
      JsonDocument doc;
      doc["protocol"] = "MITSUBISHI_HEAVY_88";
      doc["power"]    = power;
      doc["mode"]     = mode;
      doc["temp"]     = temp;
      doc["fan"]      = fan;
      char buf[256];
      serializeJson(doc, buf, sizeof(buf));
      mqttClient.publish(topicIRReceived.c_str(), buf);
      Serial.println("[MQTT] Decoded frame published");
    }

  } else if (results.decode_type == UNKNOWN) {
    // Attempt custom Mitsubishi Heavy decoding from raw timings.
    bool parsed = false;
    if (results.rawlen >= 131) {
      uint8_t frame[8] = {0};
      for (int b = 0; b < 8; b++) {
        for (int bit = 0; bit < 8; bit++) {
          int idx = 4 + (b * 8 + bit) * 2;
          if (results.rawbuf[idx] > 1250) {  // threshold: 2500us / 2us-per-tick
            frame[b] |= (1 << bit);
          }
        }
      }

      Serial.printf("[IR] Extracted frame: ");
      for (int i = 0; i < 8; i++) Serial.printf("%02X ", frame[i]);
      Serial.println();

      bool headerOk   = (frame[0] == 0xFF && frame[1] == 0x00);
      bool tailOk     = (frame[6] == 0x2A && frame[7] == 0xD5);
      bool checksumOk = (frame[5] == (uint8_t)(0xFF - frame[4]));

      if (headerOk && tailOk && checksumOk) {
        parsed = true;

        stdAc::fanspeed_t newFan;
        if      (frame[2] == 0xFF) newFan = stdAc::fanspeed_t::kHigh;
        else if (frame[2] == 0xBF) newFan = stdAc::fanspeed_t::kMedium;
        else                       newFan = stdAc::fanspeed_t::kLow;

        uint8_t tempCode = frame[4] >> 4;
        uint8_t modeCode = frame[4] & 0x0F;
        bool    newPower = !(modeCode & 0x8);
        modeCode &= 0x7;

        uint8_t newTemp = 32 - tempCode;

        stdAc::opmode_t newMode;
        switch (modeCode) {
          case 0x6: newMode = stdAc::opmode_t::kCool; break;
          case 0x7: newMode = stdAc::opmode_t::kAuto; break;
          case 0x5: newMode = stdAc::opmode_t::kDry;  break;
          case 0x4: newMode = stdAc::opmode_t::kFan;  break;
          case 0x3: newMode = stdAc::opmode_t::kHeat; break;
          default:  newMode = stdAc::opmode_t::kAuto; break;
        }

        Serial.printf("[IR] Decoded: Power=%s Mode=%d Temp=%u Fan=%d\n",
          newPower ? "ON" : "OFF", (int)newMode, newTemp, (int)newFan);

        acPower = newPower;
        acMode  = newMode;
        acTemp  = newTemp;
        acFan   = newFan;
        if (!newPower) {
          currentScene = "";
          publishSceneState();
        }
        publishACState();
      } else {
        Serial.printf("[IR] Frame validation failed: header=%d tail=%d checksum=%d\n",
          headerOk, tailOk, checksumOk);
      }
    }

    if (!parsed) {
      Serial.println("Unknown protocol, dumping raw data:");
      Serial.println(resultToSourceCode(&results));
    }
  } else {
    Serial.printf("Protocol %s is not Mitsubishi Heavy\n",
      typeToString(results.decode_type).c_str());
    Serial.println(resultToSourceCode(&results));
  }

  if (mqttClient.connected()) {
    String raw = resultToSourceCode(&results);
    if (raw.length() > 900) raw = raw.substring(0, 900);
    mqttClient.publish((topicIRReceived + "/raw").c_str(), raw.c_str());
  }

  irrecv.resume();
  Serial.println("========================================");
}

// ===================== WiFi / MQTT / OTA =====================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected, IP: "); Serial.println(WiFi.localIP());
    setLed(true);
  } else {
    Serial.println("[WiFi] Connect failed"); setLed(false);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Update starting");
    irrecv.disableIRIn();
  });
  ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] Update complete"); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Error[%u]\n", e);
    irrecv.enableIRIn();  // re-enable IR capture so a failed upload does not silently disable receive
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Service started");
}

bool connectMQTT() {
  if (mqttClient.connected()) return true;
  String clientId = String(DEVICE_ID) + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Connecting to %s:%u ...\n", MQTT_HOST, MQTT_PORT);
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                            topicAvailability.c_str(), 0, true, "offline");
  } else {
    ok = mqttClient.connect(clientId.c_str(), topicAvailability.c_str(), 0, true, "offline");
  }
  if (ok) {
    Serial.println("[MQTT] Connected");
    mqttClient.subscribe((topicACCommand + "/mode").c_str());
    mqttClient.subscribe((topicACCommand + "/temp").c_str());
    mqttClient.subscribe((topicACCommand + "/fan").c_str());
    mqttClient.subscribe((topicACCommand + "/set_all").c_str());
    mqttClient.subscribe((topicACCommand + "/preset").c_str());
    mqttClient.subscribe((topicScene + "/set").c_str());
    publishAvailability("online");
    publishDiscovery();
    publishACState();
    publishSceneState();
    return true;
  } else {
    Serial.printf("[MQTT] Connect failed, rc=%d\n", mqttClient.state()); return false;
  }
}

void ensureConnections() {
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) { setLed(false); connectWiFi(); }
  }
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    if (millis() - lastMqttReconnectAttempt > 5000) {
      lastMqttReconnectAttempt = millis(); connectMQTT();
    }
  }
}

// ===================== setup / loop =====================

void setup() {
  pinMode(WIFI_STATUS_LED, OUTPUT);
  setLed(false);
  Serial.begin(115200);
  delay(1500);

  Serial.println("====================================");
  Serial.println(" ESP32 IR AC Controller v2.0       ");
  Serial.println("====================================");

  String prefix = String(TOPIC_PREFIX);
  topicAvailability = "home/" + String(DEVICE_ID) + "/availability";
  topicIRReceived   = prefix + "/ir_received";
  topicACCommand    = prefix + "/cmd/ac_climate";
  topicACState      = prefix + "/state/ac_climate";
  topicScene        = prefix + "/cmd/scene";
  topicSceneState   = prefix + "/state/scene";

  irsend.begin();
  Serial.printf("[IR Send] init, pin=%u\n", IR_LED_PIN);

  irrecv.setUnknownThreshold(kMinUnknownSize);
  irrecv.enableIRIn();
  Serial.printf("[IR Recv] init, pin=%u\n", IR_RECV_PIN);

  connectWiFi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  setupOTA();
  connectMQTT();

  Serial.println("[BOOT] Init complete");
  Serial.println("[BOOT] Point a remote at the receiver; protocol info will appear in the serial monitor");
}

void loop() {
  ensureConnections();
  if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
  if (mqttClient.connected()) mqttClient.loop();

  handleIRReceive();

  delay(10);
}
