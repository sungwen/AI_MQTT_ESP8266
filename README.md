```
from flask import Flask, Response, render_template_string, request, jsonify
import cv2
import paho.mqtt.client as mqtt
import json
import threading
import speech_recognition as sr
import datetime

# MQTT 伺服器設定
MQTT_BROKER = "demo.thingsboard.io"
MQTT_TOPIC_TELEMETRY = "v1/devices/me/telemetry"
TOKEN = "7z0y91k7lew58lir3mlx"

# 建立 MQTT 客戶端
mqtt_client = mqtt.Client(client_id="AI_Camera", callback_api_version=1)
mqtt_client.username_pw_set(TOKEN)
mqtt_client.connect(MQTT_BROKER, 1883, 60)
mqtt_client.loop_start()

# 狀態變數
light_status = "未知"
voice_status = "未啟動"
voice_thread = None
stop_voice_control = False

# 訂閱燈光狀態
def on_message(client, userdata, message):
    global light_status
    try:
        payload = json.loads(message.payload.decode())
        if "TurnOnLight" in payload:
            light_status = "開燈" if payload["TurnOnLight"] else "關燈"
    except json.JSONDecodeError:
        print("❌ 解析 MQTT 訊息失敗")

mqtt_client.subscribe(MQTT_TOPIC_TELEMETRY)
mqtt_client.on_message = on_message

# Flask Web 伺服器
app = Flask(__name__)

# 初始化 Webcam
camera = cv2.VideoCapture(0)

# 人臉偵測模型
face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")

# 影像串流函數，包含 AI 偵測標記框
def generate_frames():
    while True:
        success, frame = camera.read()
        if not success:
            break

        # 轉換為灰階圖像以進行人臉偵測
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        faces = face_cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30))

        # 繪製標記框
        for (x, y, w, h) in faces:
            cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)

        _, buffer = cv2.imencode('.jpg', frame)
        frame = buffer.tobytes()

        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')

# AI 影像串流 API
@app.route('/video_feed')
def video_feed():
    return Response(generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

# 取得狀態 API
@app.route('/status')
def get_status():
    return jsonify(light_status=light_status, voice_status=voice_status)

# 啟動 / 停止 語音控制
@app.route('/voice_control', methods=["POST"])
def toggle_voice():
    global voice_thread, stop_voice_control, voice_status
    action = request.json.get("action")

    if action == "start":
        stop_voice_control = False
        if voice_thread is None:
            voice_thread = threading.Thread(target=voice_control, daemon=True)
            voice_thread.start()
        voice_status = "已啟動"
    elif action == "stop":
        stop_voice_control = True
        voice_thread = None
        voice_status = "已停止"

    return jsonify(status=voice_status)

# 手動開關燈 API
@app.route('/toggle_light', methods=["POST"])
def toggle_light():
    action = request.json.get("action")
    if action == "on":
        mqtt_client.publish(MQTT_TOPIC_TELEMETRY, json.dumps({"method": "setLED", "params": {"enabled": True}}))
    elif action == "off":
        mqtt_client.publish(MQTT_TOPIC_TELEMETRY, json.dumps({"method": "setLED", "params": {"enabled": False}}))
    return jsonify(status="燈光已變更")

# 語音控制函數
def voice_control():
    recognizer = sr.Recognizer()
    mic = sr.Microphone()

    while not stop_voice_control:
        with mic as source:
            print("🎤 語音控制中，請說：開燈 / 關燈...")
            recognizer.adjust_for_ambient_noise(source, duration=1)
            audio = recognizer.listen(source, timeout=5, phrase_time_limit=3)

        try:
            command = recognizer.recognize_google(audio, language="zh-TW")
            print(f"✅ 辨識成功: {command}")

            if "開燈" in command:
                mqtt_client.publish(MQTT_TOPIC_TELEMETRY, json.dumps({"method": "setLED", "params": {"enabled": True}}))
            elif "關燈" in command:
                mqtt_client.publish(MQTT_TOPIC_TELEMETRY, json.dumps({"method": "setLED", "params": {"enabled": False}}))
        except sr.UnknownValueError:
            print("❌ 語音辨識失敗")
        except sr.RequestError:
            print("❌ 無法連線至 Google Speech API")

# 網頁首頁
@app.route('/')
def home():
    return render_template_string('''
        <html>
        <head>
            <title>AI Camera & ESP8266 Control</title>
            <script>
                function updateStatus() {
                    fetch('/status')
                        .then(response => response.json())
                        .then(data => {
                            document.getElementById('status').innerText = "燈光狀態: " + data.light_status;
                            document.getElementById('voice_status').innerText = "語音控制: " + data.voice_status;
                        });
                }
                function toggleVoice(action) {
                    fetch('/voice_control', {
                        method: "POST",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({ action: action })
                    }).then(response => response.json())
                      .then(data => {
                          document.getElementById('voice_status').innerText = "語音控制: " + data.status;
                          alert("語音控制: " + data.status);
                      });
                }
                function toggleLight(action) {
                    fetch('/toggle_light', {
                        method: "POST",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({ action: action })
                    }).then(response => response.json())
                      .then(data => {
                          alert(data.status);
                      });
                }
                setInterval(updateStatus, 2000);
            </script>
        </head>
        <body>
            <h1>AI Camera & ESP8266 Control</h1>
            <img src="/video_feed"><br><br>
            <h2 id="status">燈光狀態: 讀取中...</h2>
            <h2 id="voice_status">語音控制: 未啟動</h2>
            <button onclick="toggleVoice('start')">啟動語音控制</button>
            <button onclick="toggleVoice('stop')">停止語音控制</button>
            <br><br>
            <button onclick="toggleLight('on')">開燈</button>
            <button onclick="toggleLight('off')">關燈</button>
        </body>
        </html>
    ''')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)
```
# 📌 程式碼講解
這段程式碼的功能是 透過 Flask 網頁介面 來控制 ESP8266 開關燈，並且 透過 Webcam 進行 AI 偵測 以及 語音辨識來開關燈。

## 📌 1. 導入必要的 Python 模組
```
from flask import Flask, Response, render_template_string, request, jsonify
import cv2
import paho.mqtt.client as mqtt
import json
import threading
import speech_recognition as sr
import datetime
```
* Flask：建立 Web 伺服器
* cv2 (OpenCV)：處理 Webcam 影像 + AI 人臉偵測
* paho.mqtt.client：與 ESP8266 透過 MQTT 進行通訊
* json：處理資料封裝
* threading：用來讓語音控制運行在背景
* speech_recognition：用來處理語音辨識

## 📌 2. MQTT 連線設定
```
MQTT_BROKER = "demo.thingsboard.io"
MQTT_TOPIC_TELEMETRY = "v1/devices/me/telemetry"
TOKEN = "7z0y91k7lew58lir3mlx"

mqtt_client = mqtt.Client(client_id="AI_Camera", callback_api_version=1)
mqtt_client.username_pw_set(TOKEN)
mqtt_client.connect(MQTT_BROKER, 1883, 60)
mqtt_client.loop_start()
```light_status = "未知"
voice_status = "未啟動"
voice_thread = None
stop_voice_control = False
```
* MQTT_BROKER：這是 ThingsBoard MQTT 伺服器（或其他 MQTT 伺服器的 IP）。
* TOKEN：這是 ThingsBoard 裝置授權 Token，你需要填寫 你自己的 Token。
* mqtt_client.loop_start()：讓 MQTT 持續運行，接收來自 ThingsBoard 的訊息。

## 📌 3. 燈光與語音控制的狀態變數
```
light_status = "未知"
voice_status = "未啟動"
voice_thread = None
stop_voice_control = False
```
* light_status：存儲燈光的當前狀態（開燈 / 關燈）。
* voice_status：存儲語音控制的狀態（已啟動 / 未啟動）。
* voice_thread：語音控制執行緒（Thread），確保語音辨識能夠在 背景執行。
* stop_voice_control：用來 啟動 / 停止 語音控制。

## 📌 4. 訂閱 MQTT 燈光狀態
```
def on_message(client, userdata, message):
    global light_status
    try:
        payload = json.loads(message.payload.decode())
        if "TurnOnLight" in payload:
            light_status = "開燈" if payload["TurnOnLight"] else "關燈"
    except json.JSONDecodeError:
        print("❌ 解析 MQTT 訊息失敗")

mqtt_client.subscribe(MQTT_TOPIC_TELEMETRY)
mqtt_client.on_message = on_message
```

這段程式碼會 訂閱 ThingsBoard 的 MQTT 訊息，當 ESP8266 傳回燈光狀態 時，程式會更新 light_status。

## 📌 5. Flask 伺服器初始化
```
app = Flask(__name__)

```
這是 啟動 Flask Web 伺服器。

## 📌 6. 初始化 Webcam + AI 偵測
```
camera = cv2.VideoCapture(0)
face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
```
* cv2.VideoCapture(0)：開啟 Webcam（0 代表第一個攝影機）。
* face_cascade：載入 OpenCV 人臉偵測模型，用來偵測畫面中的人臉。

## 📌 7. 影像串流（顯示 AI 偵測標記框）
```
def generate_frames():
    while True:
        success, frame = camera.read()
        if not success:
            break

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        faces = face_cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30))

        for (x, y, w, h) in faces:
            cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)

        _, buffer = cv2.imencode('.jpg', frame)
        frame = buffer.tobytes()

        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
```
這段程式碼：

* 讀取 Webcam 影像
* 轉換成 灰階圖像，進行 人臉偵測
* 如果偵測到人臉，會在 人臉周圍畫出標記框
* 影像會即時顯示在網頁上

## 📌 8. 語音控制功能
```
def voice_control():
    recognizer = sr.Recognizer()
    mic = sr.Microphone()

    while not stop_voice_control:
        with mic as source:
            print("🎤 語音控制中，請說：開燈 / 關燈...")
            recognizer.adjust_for_ambient_noise(source, duration=1)
            audio = recognizer.listen(source, timeout=5, phrase_time_limit=3)

        try:
            command = recognizer.recognize_google(audio, language="zh-TW")
            print(f"✅ 辨識成功: {command}")

            if "開燈" in command:
                mqtt_client.publish(MQTT_TOPIC_TELEMETRY, json.dumps({"method": "setLED", "params": {"enabled": True}}))
            elif "關燈" in command:
                mqtt_client.publish(MQTT_TOPIC_TELEMETRY, json.dumps({"method": "setLED", "params": {"enabled": False}}))
        except sr.UnknownValueError:
            print("❌ 語音辨識失敗")
        except sr.RequestError:
            print("❌ 無法連線至 Google Speech API")
```
這段程式碼：

* 監聽麥克風
* 辨識語音內容（「開燈」 / 「關燈」）
* 透過 MQTT 發送燈光指令到 ESP8266

## 📌 9. Flask 網頁介面
```
@app.route('/')
def home():
    return render_template_string('''
        <html>
        <head>
            <title>AI Camera & ESP8266 Control</title>
        </head>
        <body>
            <h1>AI Camera & ESP8266 Control</h1>
            <img src="/video_feed"><br><br>
            <h2 id="status">燈光狀態: 讀取中...</h2>
            <h2 id="voice_status">語音控制: 未啟動</h2>
            <button onclick="toggleVoice('start')">啟動語音控制</button>
            <button onclick="toggleVoice('stop')">停止語音控制</button>
            <br><br>
            <button onclick="toggleLight('on')">開燈</button>
            <button onclick="toggleLight('off')">關燈</button>
        </body>
        </html>
    ''')
```
* 顯示影像串流
* 顯示燈光狀態
* 顯示語音控制狀態
* 提供「手動開燈 / 關燈」按鈕
* 提供「啟動 / 停止語音控制」按鈕

## 📌 10. 啟動 Flask 伺服器
```
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)
```
這行代碼會啟動 Flask 伺服器，允許其他裝置（手機 / 電腦）存取此網頁。

## 📌 總結
這個 Flask 應用程式 結合 AI 影像辨識 + MQTT + 語音控制，可以： ✅ 手動 / 語音控制 ESP8266 燈光
✅ 即時顯示影像串流 + AI 人臉偵測標記框
✅ 燈光狀態 & 語音控制狀態即時顯示

🚀 這是完整的 AI + 物聯網 MQTT 控制方案！ 🎤💡🎥