/*
  MQTT_Relay_D0.ino
  ESP8266 + WiFiManager + MQTT 繼電器控制（ON/OFF/TOGGLE）
  - 繼電器腳位：D0（GPIO16）← 避開 D1/D2/D5/D6/D7 與開機綁定腳 D3/D4/D8
  - MQTT 主題： ttu_fish/relay  （訂閱與狀態回報同一主題）
  - 接收指令： "ON" / "OFF" / "TOGGLE" / "1" / "0"
  - 狀態回報： "ON" / "OFF"（retained = true）
  - 預設 MQTT Broker： test.mosquitto.org:1883

  提醒：
  多數常見繼電器板為「低電位觸發（Active-LOW）」。
  若你的板子是「高電位觸發」，請把 RELAY_ACTIVE_HIGH 改成 true。
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

// --------- 使用者可調區 ---------
#define RELAY_PIN         D0          // GPIO16（安全、不影響開機）
#define RELAY_ACTIVE_HIGH false       // 常見板子為低電位觸發 → false

const char* MQTT_SERVER = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;

// 單一主題（訂閱＋發佈都用這個）
const char* TOPIC_RELAY = "ttu_fish/relay";
// --------------------------------

const uint8_t LEVEL_ON  = RELAY_ACTIVE_HIGH ? HIGH : LOW;
const uint8_t LEVEL_OFF = RELAY_ACTIVE_HIGH ? LOW  : HIGH;

WiFiClient espClient;
PubSubClient mqtt(espClient);

bool relayState = false;  // false=OFF, true=ON

// ---- 小工具 ----
void applyRelay(bool on) {
  digitalWrite(RELAY_PIN, on ? LEVEL_ON : LEVEL_OFF);
  relayState = on;
}

void publishState() {
  const char* msg = relayState ? "ON" : "OFF";
  mqtt.publish(TOPIC_RELAY, msg, true);  // retained=true
  Serial.printf("[MQTT] publish state: %s (retained)\n", msg);
}

void setRelay(bool on) {
  if (relayState != on) {
    applyRelay(on);
    Serial.printf("[RELAY] %s\n", on ? "ON" : "OFF");
    if (mqtt.connected()) publishState();
  }
}

void toggleRelay() {
  setRelay(!relayState);
}

// ---- MQTT ----
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg; msg.reserve(length);
  for (unsigned int i = 0; i < length; ++i) msg += (char)payload[i];
  msg.trim();
  Serial.printf("[MQTT] recv: topic=%s, payload='%s'\n", topic, msg.c_str());

  if (msg.equalsIgnoreCase("ON") || msg == "1") {
    setRelay(true);
  } else if (msg.equalsIgnoreCase("OFF") || msg == "0") {
    setRelay(false);
  } else if (msg.equalsIgnoreCase("TOGGLE")) {
    toggleRelay();
  } else {
    Serial.println("[MQTT] unknown command (use ON/OFF/TOGGLE or 1/0)");
  }
}

void ensureMqtt() {
  while (!mqtt.connected()) {
    String cid = String("esp8266-relay-") + String(ESP.getChipId(), HEX);
    Serial.printf("[MQTT] connecting %s:%u ...\n", MQTT_SERVER, MQTT_PORT);
    if (mqtt.connect(cid.c_str())) {
      Serial.println("[MQTT] connected");
      mqtt.subscribe(TOPIC_RELAY);
      publishState(); // 上線即回報狀態（retained）
    } else {
      Serial.printf("[MQTT] connect failed, rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

// ---- Arduino 標準流程 ----
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[BOOT] MQTT Relay on D0 starting...");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LEVEL_OFF);  // 上電預設關閉

  // WiFi 自動配網（長按 AP 名稱 ESP-AutoWiFi 進入設定）
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("ESP-AutoWiFi")) {
    Serial.println("[WiFi] connect failed, restarting...");
    delay(300);
    ESP.restart();
  }
  Serial.printf("[WiFi] connected: %s\n", WiFi.localIP().toString().c_str());

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  applyRelay(false); // 確保初始為 OFF
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    // 若想掉線時自動關閉繼電器，可取消下一行註解
    // applyRelay(false);
    delay(200);
    return;
  }

  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();
}
