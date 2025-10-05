#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>

// --- WiFi and MQTT Configuration ---
const char* mqtt_server = "test.mosquitto.org"; // 請替換成您的MQTT伺服器IP
const int mqtt_port = 1883;
const char* mqtt_topic = "TTU_PH";

WiFiClient espClient;
PubSubClient client(espClient);

// --- pH Sensor Configuration ---
#define pH_SENSOR_PIN A0 // 定義GPIO腳位為ADC0
#define VREF 3.3 // ESP32輸入電壓為3.3V
#define SCOUNT 30 // 取樣數
int analogBuffer[SCOUNT];
int analogBufferIndex = 0;
float averageVoltage = 0;
float pHValue = 0;
float pHslope = -4.6322;
float pHintercept = 18.587;

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP8266_PH_Client")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}
  
void setup()
{
  Serial.begin(115200);
  pinMode(pH_SENSOR_PIN, INPUT);

  // [修改] 啟動 WiFiManager 自動配網
  // 1. 會先嘗試用已儲存的資料連線。
  // 2. 如果連線失敗 (例如找不到AP)，它會自動啟動設定入口(AP模式)。
  WiFiManager wifiManager;
  // wifiManager.resetSettings(); // 如果需要清除已儲存的WiFi設定，可以取消此行的註解
  if (!wifiManager.autoConnect("AutoConnectAP")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    // 重啟ESP並重試
    ESP.reset();
    delay(5000);
  }

  // 如果程式能執行到這裡，代表已成功連上WiFi
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // Setup MQTT
  client.setServer(mqtt_server, mqtt_port);
}
  
float lastpHValue = -1; // 定義最後的pH值為無效(-1)
  
void loop()
{
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // pH取樣
  static unsigned long analogSampleTimepoint = 0;
  if(millis() - analogSampleTimepoint > 40)
  {
    analogSampleTimepoint = millis();
    analogBuffer[analogBufferIndex] = analogRead(pH_SENSOR_PIN);
    analogBufferIndex++;
    if(analogBufferIndex == SCOUNT)
    { 
      analogBufferIndex = 0;
    }
  }   
    
  // pH計算
  static unsigned long printTimepoint = 0;
  if(millis() - printTimepoint > 800)
  {
    printTimepoint = millis();
    float sum = 0;
    for(int i = 0; i < SCOUNT; i++)
    {
      sum += analogBuffer[i];
    }
    averageVoltage = (sum / SCOUNT) * (float)VREF / 4095; // 轉換為電壓值，ESP32的ADC解析度是12位(4095)。
  
    pHValue = averageVoltage * pHslope + pHintercept;
      
    if (abs(pHValue - lastpHValue) > 0.1) // 如果pH數值變化大於0.1，才會更新並發布
    {
      lastpHValue = pHValue;
      Serial.print("pH Value: ");
      Serial.println(pHValue, 2);

      // Publish to MQTT topic
      char phString[8];
      dtostrf(pHValue, 4, 2, phString); // 將浮點數轉換為字串
      client.publish(mqtt_topic, phString);
      Serial.print("Published to ");
      Serial.print(mqtt_topic);
      Serial.print(": ");
      Serial.println(phString);
    }
  }
}
