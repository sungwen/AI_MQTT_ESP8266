/*
  MQTT_Relay_Simple.ino
  精簡版：WiFiManager + PubSubClient
  - Relay 腳位：D0
  - 單一主題：ttu_fish/relay（訂閱命令 + 回報狀態(保留)）
  - 指令：ON / OFF / TOGGLE / 1 / 0
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

// ====== 硬體與 MQTT 參數 ======
#define RELAY_PIN D0

const char* MQTT_SERVER = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;            // 標準 MQTT TCP 連接埠
const char* TOPIC_RELAY = "ttu_fish/relay"; // 單一主題（訂閱命令 + 回報狀態）

// ====== 全域物件 ======
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// 狀態
bool relayState = false;

// ====== 佈告狀態（retain=true）======
void publishRelayState() {
  const char* msg = relayState ? "ON" : "OFF";
  mqttClient.publish(TOPIC_RELAY, msg, true);  // 回報到同一主題，保留
  Serial.printf("[MQTT] publish %s -> %s (retained)\n", TOPIC_RELAY, msg);
}

// ====== 繼電器控制 ======
void setRelay(bool on) {
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  if (relayState != on) {
    relayState = on;
    Serial.printf("[RELAY] %s\n", on ? "ON" : "OFF");
    if (mqttClient.connected()) publishRelayState();
  }
}
void toggleRelay() { setRelay(!relayState); }

// ====== MQTT 回呼 ======
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();

  Serial.printf("[MQTT] %s <= %s\n", topic, msg.c_str());

  if (msg == "ON" || msg == "1") {
    setRelay(true);
  } else if (msg == "OFF" || msg == "0") {
    setRelay(false);
  } else if (msg == "TOGGLE") {
    toggleRelay();
  } else {
    Serial.println("[MQTT] unknown command (use ON/OFF/TOGGLE or 1/0)");
  }
}

// ====== MQTT 連線 ======
void connectMQTT() {
  if (mqttClient.connected()) return;

  Serial.print("[MQTT] connecting...");
  String clientId = "esp8266-relay-" + String(ESP.getChipId(), HEX);

  // 可選：LWT 發佈到同一主題（避免覆蓋狀態，這裡不使用 retained）
  const char* willTopic = TOPIC_RELAY;
  const char* willMsg   = "OFFLINE";
  bool willRetain = false;
  uint8_t willQos = 0;

  if (mqttClient.connect(clientId.c_str(), NULL, NULL, willTopic, willQos, willRetain, willMsg)) {
    Serial.println("connected");
    mqttClient.subscribe(TOPIC_RELAY);
    publishRelayState(); // 上線後主動回報目前狀態（retained）
  } else {
    Serial.printf("failed, rc=%d\n", mqttClient.state());
  }
}

// ====== Arduino 生命周期 ======
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 上電預設關閉
  relayState = false;

  // Wi-Fi 設定（自動入口，必要時開 AP）
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("ESP-Relay")) {
    Serial.println("[WiFi] connect fail, restarting...");
    ESP.restart();
  }
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    // Wi-Fi 斷線時：僅等待 WiFiManager 的自動重連機制
    delay(200);
    return;
  }

  if (!mqttClient.connected()) {
    connectMQTT();
    // 避免卡住 CPU：若連不上，短暫等待再 loop()
    if (!mqttClient.connected()) { delay(500); return; }
  }

  mqttClient.loop();
}
