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

// ===================== 用户配置区 =====================
#include "secrets.h"


const char* MQTT_HOST = "rasp2.local";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "";
const char* MQTT_PASS = "";

const char* DEVICE_ID = "esp32_ir_light_01";
const char* DEVICE_NAME = "ESP32 IR Light";
const char* OTA_HOSTNAME = "esp32-ir-light";

const uint16_t IR_LED_PIN = 4;
// ⚠️ 根据你的模块修改这个引脚！
// 看板子上 IR RX 标注对应的 GPIO，常见是 GPIO15 或 GPIO14
const uint16_t IR_RECV_PIN = 14;
const uint16_t WIFI_STATUS_LED = 2;

// ===================== IR 接收配置 =====================
const uint16_t kCaptureBufferSize = 1024;
const uint8_t kTimeout = 50;      // ms，信号超时判断
const uint16_t kMinUnknownSize = 12;

// ===================== AC 状态（用于 IRac 发送）=====================
bool acPower = false;
stdAc::opmode_t acMode = stdAc::opmode_t::kCool;
float acTemp = 26.0;
stdAc::fanspeed_t acFan = stdAc::fanspeed_t::kAuto;

// ===================== MQTT Topic =====================
String topicCommand;
String topicAvailability;
String topicDiscovery;
String topicIRReceived;
String topicACCommand;
String topicACState;
String topicScene;
String topicSceneState;

// ===================== 场景状态追踪 =====================
String currentScene = "";  // "" | "power_blast" | "comfort" | "sleep" | "leave"

// ===================== 全局对象 =====================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
IRsend irsend(IR_LED_PIN);
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kTimeout, true);
decode_results results;

unsigned long lastIRSendTime = 0;
const uint16_t IR_IGNORE_WINDOW_MS = 500;

unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastWifiCheck = 0;

// ===================== 工具函数 =====================
void publishACState();
void publishSceneState();
void setLed(bool on) {
  digitalWrite(WIFI_STATUS_LED, on ? HIGH : LOW);
}
// ===================== 自定义三菱重工协议发送 =====================
// 协议逆向结果：
//   B0=FF  B1=00  B2=风速  B3=风速补  B4=温度|模式  B5=0xFF-B4  B6=2A  B7=D5  B8=00
//
// 风速 B2/B3:  高=FF/00  中=BF/40  低=9F/60
// 模式 B4低4:  Cool=6  Dry=5  Fan=4  Heat=3  (关机时 |0x8)
// 温度 B4高4:  (32 - temp) & 0xF

void sendMitsubishiCustom(bool power, uint8_t mode_code,
                          uint8_t temp, uint8_t fan_b2, uint8_t fan_b3) {
  uint8_t mc = mode_code;
  if (!power) mc |= 0x8;  // 关机：bit3置1

  uint8_t temp_code = (32 - temp) & 0xF;
  uint8_t b4 = (temp_code << 4) | mc;
  uint8_t b5 = 0xFF - b4;

  uint8_t frame[8] = {0xFF, 0x00, fan_b2, fan_b3, b4, b5, 0x2A, 0xD5};

  Serial.printf("[IR] 发送帧: ");
  for (int i = 0; i < 8; i++) Serial.printf("%02X ", frame[i]);
  Serial.println();

  // 构造 raw 时序数组
  // Header: 5950us HIGH + 7475us LOW
  // Bit1  : 508us HIGH  + 3454us LOW
  // Bit0  : 508us HIGH  + 1496us LOW
  // Trailer: 508us HIGH + 7420us LOW
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



  // 连发3次
  for (int r = 0; r < 3; r++) {
    irsend.sendRaw(raw, idx, 38);
    delay(50);
  }
  Serial.println("[IR] 发送完成");

  lastIRSendTime = millis();

}

// 对外接口，参数与原来保持一致
void sendACCommand(bool power, stdAc::opmode_t mode, float temp, stdAc::fanspeed_t fan) {
  Serial.printf("[AC] Power=%s Mode=%d Temp=%.0f Fan=%d\n",
    power ? "ON" : "OFF", (int)mode, temp, (int)fan);

  // 模式编码
  uint8_t mc;
  switch (mode) {
    case stdAc::opmode_t::kCool: mc = 0x6; break;
    case stdAc::opmode_t::kDry:  mc = 0x5; break;
    case stdAc::opmode_t::kFan:  mc = 0x4; break;
    case stdAc::opmode_t::kHeat: mc = 0x3; break;
    default:                     mc = 0x6; break;  // Auto → Cool
  }

  // 风速编码
  uint8_t fb2, fb3;
  switch (fan) {
    case stdAc::fanspeed_t::kMax:
    case stdAc::fanspeed_t::kHigh:   fb2 = 0xFF; fb3 = 0x00; break;
    case stdAc::fanspeed_t::kMedium: fb2 = 0xBF; fb3 = 0x40; break;
    default:                          fb2 = 0x9F; fb3 = 0x60; break;  // Low/Auto
  }

  // 温度范围限制
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

  const char* fanStr = "auto";
  switch (acFan) {
    case stdAc::fanspeed_t::kMax:
    case stdAc::fanspeed_t::kHigh:   fanStr = "high"; break;
    case stdAc::fanspeed_t::kMedium: fanStr = "medium"; break;
    case stdAc::fanspeed_t::kLow:    fanStr = "low"; break;
    default:                         fanStr = "auto"; break;
  }

  JsonDocument doc;
  doc["mode"] = modeStr;
  doc["temperature"] = acTemp;
  doc["fan_mode"] = fanStr;
  doc["preset_mode"] = "none";
  if (currentScene == "power_blast") doc["preset_mode"] = "强力制冷";
  else if (currentScene == "comfort") doc["preset_mode"] = "舒适模式";
  else if (currentScene == "sleep") doc["preset_mode"] = "静音睡眠";
  else if (currentScene == "leave") doc["preset_mode"] = "离家关闭";

  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(topicACState.c_str(), buf, true);  // retain=true
  Serial.printf("[MQTT] State: %s\n", buf);
}

void publishSceneState() {
  if (!mqttClient.connected()) return;

  // 将 internal key 映射为 HA select 显示用的中文名
  const char* displayName = "已关机";
  if (currentScene == "power_blast") displayName = "强力制冷";
  else if (currentScene == "comfort") displayName = "舒适模式";
  else if (currentScene == "sleep")   displayName = "静音睡眠";
  else if (currentScene == "leave")   displayName = "离家关闭";

  mqttClient.publish(topicSceneState.c_str(), displayName, true);
  Serial.printf("[MQTT] Scene: %s\n", displayName);
}

void publishDiscovery() {
  // 露营灯按钮
  {
    JsonDocument doc;
    doc["name"]                  = "露营灯";
    doc["unique_id"]             = "camping_light_toggle";
    doc["command_topic"]         = topicCommand;
    doc["payload_press"]         = "PRESS";
    doc["availability_topic"]    = "camping_ir/camping_light/availability";
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add(DEVICE_ID);
    device["name"]         = DEVICE_NAME;
    device["manufacturer"] = "Bluebird";
    device["model"]        = "ESP32 IR Controller";
    char payload[512];
    serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(
      "homeassistant/button/camping_ir/camping_light_toggle/config",
      payload, true
    );
  }

  // Climate 实体发现（新增）
  {
    JsonDocument doc;
    doc["name"] = "三菱空调";
    doc["unique_id"] = "mitsubishi_ac_climate";
    doc["availability_topic"]    = topicAvailability;
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";

    // 模式指令
    doc["mode_command_topic"] = topicACCommand + "/mode";

    // 温度指令
    doc["temperature_command_topic"] = topicACCommand + "/temp";

    // 状态回传(让 HA 界面跟着变)
    doc["mode_state_topic"] = topicACState;
    doc["mode_state_template"] = "{{ value_json.mode }}";
    doc["temperature_state_topic"] = topicACState;
    doc["temperature_state_template"] = "{{ value_json.temperature }}";
    doc["current_temperature_topic"] = "camping_ir/sensor/room/temperature";

    // 风速指令 / 状态
    doc["fan_mode_command_topic"] = topicACCommand + "/fan";
    doc["fan_mode_state_topic"] = topicACState;
    doc["fan_mode_state_template"] = "{{ value_json.fan_mode }}";

    // 支持的模式
    JsonArray modes = doc["modes"].to<JsonArray>();
    modes.add("off");
    modes.add("cool");
    modes.add("heat");
    modes.add("fan_only");
    modes.add("dry");
    modes.add("auto");

    JsonArray fan_modes = doc["fan_modes"].to<JsonArray>();
    fan_modes.add("high");
    fan_modes.add("medium");
    fan_modes.add("low");
    fan_modes.add("auto");

    // 场景预设按钮（显示在 climate 卡片上，按了高亮）
    doc["preset_mode_command_topic"] = topicACCommand + "/preset";
    doc["preset_mode_state_topic"]   = topicACState;
    doc["preset_mode_state_template"] = "{{ value_json.preset_mode }}";
    JsonArray presets = doc["preset_modes"].to<JsonArray>();
    presets.add("强力制冷");
    presets.add("舒适模式");
    presets.add("静音睡眠");
    presets.add("离家关闭");

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
      Serial.println("[MQTT] ⚠️  payload 可能被截断！");
    }
    bool ok = mqttClient.publish(
      "homeassistant/climate/camping_ir/mitsubishi_ac/config",
      payload, true
    );
    Serial.printf("[MQTT] Climate discovery publish: %s\n", ok ? "OK" : "FAILED");
  }
}

// 预留：旧版 JSON 控制入口
// 当前 HA climate 使用分离的 mode/temp/fan topic，此函数暂未使用
void handleACJson(const String& json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("[AC] JSON 解析失败");
    return;
  }

  if (!doc["mode"].isNull()) {
    String mode = doc["mode"].as<String>();
    if (mode == "off") {
      acPower = false;
      currentScene = "";
    } else {
      acPower = true;
      if (mode == "cool")      acMode = stdAc::opmode_t::kCool;
      else if (mode == "heat") acMode = stdAc::opmode_t::kHeat;
      else if (mode == "fan_only") acMode = stdAc::opmode_t::kFan;
      else if (mode == "dry")  acMode = stdAc::opmode_t::kDry;
      else                     acMode = stdAc::opmode_t::kAuto;
    }
  }

  if (!doc["temperature"].isNull()) {
    acTemp = doc["temperature"].as<float>();
  }

  sendACCommand(acPower, acMode, acTemp, acFan);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] 收到 %s : %s\n", topic, msg.c_str());

  if (String(topic) == topicCommand && msg == "PRESS") {
    Serial.println("[露营灯] toggle 按钮触发(IR 发送待实现)");
    // TODO: sendCampingLightToggle() - 需要先找回或录制露营灯 IR 码
  }
  String t = String(topic);

  // 模式切换
  if (t == topicACCommand + "/mode") {
    if (msg == "off") {
      acPower = false;
      currentScene = "";
      publishSceneState();
    } else {
      if      (msg == "cool")     acMode = stdAc::opmode_t::kCool;
      else if (msg == "heat")     acMode = stdAc::opmode_t::kHeat;
      else if (msg == "dry")      acMode = stdAc::opmode_t::kDry;
      else if (msg == "fan_only") acMode = stdAc::opmode_t::kFan;
      else                        acMode = stdAc::opmode_t::kAuto;
    }
    Serial.printf("[AC] Mode 指令: %s\n", msg.c_str());
    sendACCommand(acPower, acMode, acTemp, acFan);
  }
  // 温度设置
  else if (t == topicACCommand + "/temp") {
    acTemp = msg.toFloat();
    Serial.printf("[AC] Temp 指令: %.1f\n", acTemp);
    sendACCommand(acPower, acMode, acTemp, acFan);
  }
  else if (t == topicACCommand + "/fan") {
    if (msg == "high")        acFan = stdAc::fanspeed_t::kHigh;
    else if (msg == "medium") acFan = stdAc::fanspeed_t::kMedium;
    else if (msg == "low")    acFan = stdAc::fanspeed_t::kLow;
    else                       acFan = stdAc::fanspeed_t::kAuto;
    Serial.printf("[AC] Fan 指令: %s\n", msg.c_str());
    sendACCommand(acPower, acMode, acTemp, acFan);
  }
  // 场景预设按钮指令（来自 climate 卡片 preset 按钮或 select 实体）
  else if (t == topicACCommand + "/preset" || t == topicScene + "/set") {
    String sceneName = msg;
    sceneName.trim();
    Serial.printf("[Scene] Select 指令: %s\n", sceneName.c_str());

    if (sceneName == "强力制冷") {
      currentScene = "power_blast";
      acPower = true; acMode = stdAc::opmode_t::kCool;
      acFan = stdAc::fanspeed_t::kHigh; acTemp = 16;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else if (sceneName == "舒适模式") {
      currentScene = "comfort";
      acPower = true; acMode = stdAc::opmode_t::kCool;
      acFan = stdAc::fanspeed_t::kMedium; acTemp = 26;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else if (sceneName == "静音睡眠") {
      currentScene = "sleep";
      acPower = true; acMode = stdAc::opmode_t::kCool;
      acFan = stdAc::fanspeed_t::kLow; acTemp = 28;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else if (sceneName == "离家关闭") {
      currentScene = "leave";
      acPower = false;
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
    else {
      currentScene = "";
    }
    publishSceneState();
  }
  // 一次性设置所有参数（agent set_climate 用）
  else if (t == topicACCommand + "/set_all") {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      Serial.printf("[set_all] JSON 解析失败: %s\n", err.c_str());
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
      acTemp = doc["temperature"].as<float>();
      changed = true;
    }
    if (!doc["fan_speed"].isNull()) {
      String fan = doc["fan_speed"].as<String>();
      if      (fan == "high")   acFan = stdAc::fanspeed_t::kHigh;
      else if (fan == "medium") acFan = stdAc::fanspeed_t::kMedium;
      else if (fan == "low")    acFan = stdAc::fanspeed_t::kLow;
      else                      acFan = stdAc::fanspeed_t::kAuto;
      changed = true;
    }
    if (changed) {
      Serial.printf("[set_all] mode=%s temp=%.1f fan=%d power=%d\n",
        doc["mode"].as<String>().c_str(), acTemp, (int)acFan, (int)acPower);
      sendACCommand(acPower, acMode, acTemp, acFan);
    }
  }
}

// ===================== IR 接收处理 =====================

void handleIRReceive() {
  if (!irrecv.decode(&results)) return;

  if (millis() - lastIRSendTime < IR_IGNORE_WINDOW_MS) {
    irrecv.resume();
    return;
  }

  Serial.println("========== 收到红外信号 ==========");
  Serial.printf("协议: %s\n", typeToString(results.decode_type).c_str());
  Serial.printf("原始长度: %u\n", results.rawlen - 1);
  Serial.println(resultToHumanReadableBasic(&results));

  bool isMitsubishiHeavy152 = results.decode_type == MITSUBISHI_HEAVY_152;
  bool isMitsubishiHeavy88  = results.decode_type == MITSUBISHI_HEAVY_88;

  if (isMitsubishiHeavy152) {
    Serial.println("✅ 确认是三菱重工 152bit 协议！IRac 库可以直接控制");

    IRMitsubishiHeavy152Ac acDecoder(IR_LED_PIN);
    acDecoder.setRaw(results.state);

    bool   power = acDecoder.getPower();
    uint8_t mode = acDecoder.getMode();
    float   temp = acDecoder.getTemp();
    uint8_t fan  = acDecoder.getFan();

    Serial.printf("  Power: %s\n", power ? "ON" : "OFF");
    Serial.printf("  Mode:  %u\n", mode);
    Serial.printf("  Temp:  %.1f°C\n", temp);
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
      Serial.println("[MQTT] 解析结果已上报");
    }

  } else if (isMitsubishiHeavy88) {
    Serial.println("✅ 确认是三菱重工 88bit 协议！IRac 库可以直接控制");

    IRMitsubishiHeavy88Ac acDecoder(IR_LED_PIN);
    acDecoder.setRaw(results.state);

    bool   power = acDecoder.getPower();
    uint8_t mode = acDecoder.getMode();
    float   temp = acDecoder.getTemp();
    uint8_t fan  = acDecoder.getFan();

    Serial.printf("  Power: %s\n", power ? "ON" : "OFF");
    Serial.printf("  Mode:  %u\n", mode);
    Serial.printf("  Temp:  %.1f°C\n", temp);
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
      Serial.println("[MQTT] 解析结果已上报");
    }

  } else if (results.decode_type == UNKNOWN) {
    // ---- 尝试自定义三菱重工协议解析 ----
    // 帧结构：Header(2) + 64bits×2 + trailing = 131+ entries
    bool parsed = false;
    if (results.rawlen >= 131) {
      // 从 rawbuf 提取 8 字节（LSB first，rawbuf 单位是 RAWTICK=2us）
      uint8_t frame[8] = {0};
      for (int b = 0; b < 8; b++) {
        for (int bit = 0; bit < 8; bit++) {
          int idx = 4 + (b * 8 + bit) * 2;  // rawbuf[0]=0,rawbuf[1]=HDR_MARK,rawbuf[2]=HDR_SPACE,rawbuf[3]=first BIT_MARK,rawbuf[4]=first space
          if (results.rawbuf[idx] > 1250) {        // 阈值：2500us / 2tick = 1250
            frame[b] |= (1 << bit);
          }
        }
      }

      // 打印原始帧便于调试
      Serial.printf("[IR] 提取帧: ");
      for (int i = 0; i < 8; i++) Serial.printf("%02X ", frame[i]);
      Serial.println();

      // 验证帧合法性
      bool headerOk   = (frame[0] == 0xFF && frame[1] == 0x00);
      bool tailOk     = (frame[6] == 0x2A && frame[7] == 0xD5);
      bool checksumOk = (frame[5] == (uint8_t)(0xFF - frame[4]));

      if (headerOk && tailOk && checksumOk) {
        parsed = true;

        // 解析风速（B2/B3）
        stdAc::fanspeed_t newFan;
        if      (frame[2] == 0xFF) newFan = stdAc::fanspeed_t::kHigh;
        else if (frame[2] == 0xBF) newFan = stdAc::fanspeed_t::kMedium;
        else                       newFan = stdAc::fanspeed_t::kLow;

        // 解析温度和模式（B4）
        uint8_t tempCode = frame[4] >> 4;
        uint8_t modeCode = frame[4] & 0x0F;
        bool    newPower = !(modeCode & 0x8);  // bit3=1 表示关机
        modeCode &= 0x7;                        // 去掉关机 bit

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

        Serial.printf("[IR] 解析成功: Power=%s Mode=%d Temp=%u Fan=%d\n",
          newPower ? "ON" : "OFF", (int)newMode, newTemp, (int)newFan);

        // 更新全局状态并同步到 HA
        acPower = newPower;
        acMode  = newMode;
        acTemp  = (float)newTemp;
        acFan   = newFan;
        if (!newPower) {
          currentScene = "";
          publishSceneState();
        }
        publishACState();
      } else {
        Serial.printf("[IR] 帧校验失败: header=%d tail=%d checksum=%d\n",
          headerOk, tailOk, checksumOk);
      }
    }

    if (!parsed) {
      Serial.println("⚠️  未知协议，打印 Raw 数据：");
      Serial.println(resultToSourceCode(&results));
    }
  } else {
    Serial.printf("ℹ️  协议: %s，不是三菱重工\n",
      typeToString(results.decode_type).c_str());
    Serial.println(resultToSourceCode(&results));
  }

  // 无论什么协议都上报原始数据
  if (mqttClient.connected()) {
    String raw = resultToSourceCode(&results);
    if (raw.length() > 900) raw = raw.substring(0, 900);
    mqttClient.publish((topicIRReceived + "/raw").c_str(), raw.c_str());
  }

  irrecv.resume();
  Serial.println("==================================");
}
// ===================== WiFi / MQTT / OTA（与原代码相同）=====================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] 连接到 %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] 已连接, IP: "); Serial.println(WiFi.localIP());
    setLed(true);
  } else {
    Serial.println("[WiFi] 连接失败"); setLed(false);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] 开始更新");
    irrecv.disableIRIn();
  });
  ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] 更新完成"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] 错误[%u]\n", e); });
  ArduinoOTA.begin();
  Serial.println("[OTA] 服务已启动");
}

bool connectMQTT() {
  if (mqttClient.connected()) return true;
  String clientId = String(DEVICE_ID) + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] 连接到 %s:%u ...\n", MQTT_HOST, MQTT_PORT);
  bool ok = mqttClient.connect(clientId.c_str(), topicAvailability.c_str(), 0, true, "offline");
  if (ok) {
    Serial.println("[MQTT] 连接成功");
    mqttClient.subscribe(topicCommand.c_str());
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
    Serial.printf("[MQTT] 连接失败, rc=%d\n", mqttClient.state()); return false;
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

  topicCommand    = "camping_ir/cmd/camping_light/toggle";
  topicAvailability = "home/" + String(DEVICE_ID) + "/availability";
  topicDiscovery  = "homeassistant/button/camping_ir/camping_light_toggle/config";
  topicIRReceived = "camping_ir/ir_received";
  topicACCommand  = "camping_ir/cmd/ac_climate";
  topicACState    = "camping_ir/state/ac_climate";
  topicScene      = "camping_ir/cmd/scene";
  topicSceneState = "camping_ir/state/scene";

  irsend.begin();
  Serial.printf("[IR Send] 初始化, pin=%u\n", IR_LED_PIN);

  // 启动红外接收
  irrecv.setUnknownThreshold(kMinUnknownSize);
  irrecv.enableIRIn();
  Serial.printf("[IR Recv] 初始化, pin=%u\n", IR_RECV_PIN);

  connectWiFi();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  setupOTA();
  connectMQTT();

  Serial.println("[BOOT] 初始化完成");
  Serial.println("[BOOT] 对准遥控器按按钮，Serial Monitor 会显示协议信息");
}

void loop() {
  ensureConnections();
  if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
  if (mqttClient.connected()) mqttClient.loop();

  // 处理红外接收
  handleIRReceive();

  delay(10);
}
