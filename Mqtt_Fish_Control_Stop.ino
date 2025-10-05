#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

// 馬達驅動腳位（L298N：IN1/IN2）
#define IN1 D1              // GPIO5
#define IN2 D2              // GPIO4

// WiFi 重置按鍵（避免用 D3=GPIO0 造成開機進燒錄模式）
#define RESET_PIN D7        // GPIO13（上拉輸入，按下接 GND）

// 限位開關：使用 NO + COM 接 GND，內部上拉
#define LIMIT_SWITCH_FORWARD D5   // GPIO14：正轉端極限
#define LIMIT_SWITCH_REVERSE D6   // GPIO12：反轉端極限

// MQTT
const char* mqtt_server = "test.mosquitto.org";
const char* topic_cmd    = "ttu_fish/motor1";
const char* topic_status = "ttu_fish/motor1/status";

WiFiClient espClient;
PubSubClient client(espClient);

// ====== 狀態 ======
enum MotorDirection { STOPPED, FORWARD, REVERSE };
MotorDirection currentMotorDirection = STOPPED;

enum SequenceState { SEQ_IDLE, SEQ_FORWARD, SEQ_REVERSE };
SequenceState sequenceState = SEQ_IDLE;
unsigned long sequenceStartTime = 0;

// 限位（含消抖）
bool isForwardLimit = false, isReverseLimit = false;
unsigned long fwdLastChange = 0, revLastChange = 0;
const unsigned long DEBOUNCE_MS = 20;

// 退避（避免頂在限位上）
const unsigned long BACKOFF_MS = 200;
unsigned long backoffUntil = 0;

// ====== 小工具 ======
void publishStatus(const String& s) {
  client.publish(topic_status, s.c_str(), true);
}

void stopMotor() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  if (currentMotorDirection != STOPPED) {
    currentMotorDirection = STOPPED;
    publishStatus("STOP");
    Serial.println("[MOTOR] STOP");
  }
}

void moveForward() {
  if (isForwardLimit || millis() < backoffUntil) {
    stopMotor();
    return;
  }
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  if (currentMotorDirection != FORWARD) {
    currentMotorDirection = FORWARD;
    publishStatus("FORWARD");
    Serial.println("[MOTOR] FORWARD");
  }
}

void moveReverse() {
  if (isReverseLimit || millis() < backoffUntil) {
    stopMotor();
    return;
  }
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  if (currentMotorDirection != REVERSE) {
    currentMotorDirection = REVERSE;
    publishStatus("REVERSE");
    Serial.println("[MOTOR] REVERSE");
  }
}

// ====== 限位讀取（消抖）======
void readLimitSwitches() {
  int rawF = digitalRead(LIMIT_SWITCH_FORWARD); // LOW = 觸發
  int rawR = digitalRead(LIMIT_SWITCH_REVERSE);

  unsigned long now = millis();

  static int lastF = HIGH, lastR = HIGH;
  if (rawF != lastF) { fwdLastChange = now; lastF = rawF; }
  if (rawR != lastR) { revLastChange = now; lastR = rawR; }

  if ((now - fwdLastChange) > DEBOUNCE_MS) {
    isForwardLimit = (rawF == LOW);
  }
  if ((now - revLastChange) > DEBOUNCE_MS) {
    isReverseLimit = (rawR == LOW);
  }
}

// ====== 觸發時處理 + 退避 ======
void handleLimitActions() {
  if (currentMotorDirection == FORWARD && isForwardLimit) {
    Serial.println("[LIMIT] FORWARD limit hit -> backoff & reverse");
    publishStatus("LIMIT_FORWARD");
    // 短暫退避
    backoffUntil = millis() + BACKOFF_MS;
    moveReverse();
    if (sequenceState == SEQ_FORWARD) {
      sequenceState = SEQ_REVERSE;
      sequenceStartTime = millis();
    }
  } else if (currentMotorDirection == REVERSE && isReverseLimit) {
    Serial.println("[LIMIT] REVERSE limit hit -> backoff & forward");
    publishStatus("LIMIT_REVERSE");
    backoffUntil = millis() + BACKOFF_MS;
    moveForward();
    if (sequenceState == SEQ_REVERSE) {
      sequenceState = SEQ_IDLE;
    }
  }
}

// ====== 非阻塞序列（3：正3秒→反3秒→停）======
void handleSequence() {
  if (sequenceState == SEQ_IDLE) return;

  unsigned long now = millis();
  if (sequenceState == SEQ_FORWARD) {
    if (now - sequenceStartTime >= 3000) {
      Serial.println("[SEQ] forward 3s done -> reverse");
      sequenceState = SEQ_REVERSE;
      sequenceStartTime = now;
      moveReverse();
    }
  } else if (sequenceState == SEQ_REVERSE) {
    if (now - sequenceStartTime >= 3000) {
      Serial.println("[SEQ] reverse 3s done -> stop");
      sequenceState = SEQ_IDLE;
      stopMotor();
    }
  }
}

// ====== 指令處理（0/1/2/3 & 99:遠端清 WiFi）======
void handleMotorCommand(int cmd) {
  sequenceState = SEQ_IDLE;

  switch (cmd) {
    case 0: stopMotor(); break;
    case 1: moveForward(); break;
    case 2: moveReverse(); break;
    case 3:
      Serial.println("[CMD] sequence 3 start");
      publishStatus("SEQ_START");
      sequenceState = SEQ_FORWARD;
      sequenceStartTime = millis();
      moveForward();
      break;
    case 99: { // 遠端清 WiFi（小心使用）
      publishStatus("RESET_WIFI");
      WiFiManager wm;
      wm.resetSettings();
      delay(200);
      ESP.restart();
      break;
    }
    default:
      Serial.printf("[CMD] unknown: %d\n", cmd);
      publishStatus("UNKNOWN_CMD");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    char c = (char)payload[i];
    if ((c >= '0' && c <= '9')) msg += c;
  }
  int cmd = msg.toInt();
  Serial.printf("[MQTT] cmd: %d\n", cmd);
  handleMotorCommand(cmd);
}

void connectMQTT() {
  while (!client.connected()) {
    String id = "esp8266-motor-" + String(ESP.getChipId(), HEX);
    if (client.connect(id.c_str())) {
      client.subscribe(topic_cmd);
      publishStatus("MQTT_CONNECTED");
      Serial.println("[MQTT] connected");
    } else {
      Serial.printf("[MQTT] fail rc=%d, retry...\n", client.state());
      stopMotor(); // 失聯保護
      delay(1500);
    }
  }
}

// ====== Setup / Loop ======
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] Motor controller + WiFiManager + LimitSwitch + ResetWiFi");

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_FORWARD, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_REVERSE, INPUT_PULLUP);
  stopMotor();

  // 上電時按下 Reset 鍵 -> 清除 WiFi 紀錄
  delay(80);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("[BOOT] Reset button held -> clear WiFi settings");
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("ESP-AutoWiFi")) {
    Serial.println("[WiFi] connect failed, restarting...");
    stopMotor();
    ESP.restart();
  }

  Serial.printf("[WiFi] connected: %s\n", WiFi.localIP().toString().c_str());
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);
}

void loop() {
  // 連線維護 + 失聯保護
  if (WiFi.status() != WL_CONNECTED) {
    stopMotor();
  } else {
    if (!client.connected()) connectMQTT();
    client.loop();
  }

  readLimitSwitches();
  handleLimitActions();
  handleSequence();
}
