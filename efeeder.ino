#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <RTClib.h>

// --- Konfigurasi WiFi ---
#define WIFI_SSID "Efeeder"
#define WIFI_PASSWORD "11111111"

// --- Konfigurasi MQTT ---
const char* mqtt_server = "cd635dbefa6440a2bcd152010f0841b3.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "gee2k_";
const char* mqtt_password = "@Glenpemain04";
#define MQTT_TOPIC_TIME "/efeeder/time"
#define MQTT_TOPIC_SERVO "/efeeder/servo"
#define MQTT_TOPIC_RELAY "/efeeder/relay"
#define MQTT_TOPIC_FEED_STATUS "/efeeder/feed_status"
#define MQTT_TOPIC_FED_PREFIX "/efeeder/fed"
#define MQTT_TOPIC_LOG "/efeeder/log"
#define MQTT_TOPIC_COMMAND "/efeeder/command"

// --- Waktu dan RTC ---
RTC_DS3231 rtc;

// --- Servo & Relay ---
Servo servo;
#define SERVO_PIN 21
#define RELAY_PIN 25
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// --- Ultrasonik ---
#define TRIG_PIN 5
#define ECHO_PIN 18

// --- Timer & Status ---
bool servoIsOpen = false;
unsigned long servoOpenTime = 0;
const unsigned long SERVO_DURATION = 8000;
unsigned long mqtt_lasttime = 0;
const unsigned long MQTT_MTBS = 5000;

bool alreadyFed[5] = {false, false, false, false, false};
bool hasResetToday = false;
int lastCheckedMinute = -1;
String feedingLog = "";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// --- Fungsi untuk ambil waktu ---
String getTimeString() {
  DateTime now = rtc.now();
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return String(buf);
}

// --- Fungsi membuka pakan ---
void openFeeding(String source) {
  digitalWrite(RELAY_PIN, RELAY_ON);
  servo.write(90);
  servoIsOpen = true;
  servoOpenTime = millis();
  feedingLog += "✅ " + getTimeString() + " (" + source + ")\n";

  client.publish(MQTT_TOPIC_SERVO, "Terbuka");
  client.publish(MQTT_TOPIC_RELAY, "ON");
  client.publish(MQTT_TOPIC_LOG, feedingLog.c_str());
}

// --- Fungsi reset harian ---
void resetDailySchedule() {
  DateTime now = rtc.now();
  if (now.hour() == 0 && now.minute() == 0 && !hasResetToday) {
    for (int i = 0; i < 5; i++) alreadyFed[i] = false;
    feedingLog = "";
    hasResetToday = true;
    client.publish(MQTT_TOPIC_LOG, feedingLog.c_str());
  } else if (now.hour() != 0 || now.minute() != 0) {
    hasResetToday = false;
  }
}

// --- Jadwal Otomatis ---
void checkAutoFeeding() {
  DateTime now = rtc.now();
  int h = now.hour();
  int m = now.minute();

  if (m == lastCheckedMinute) return;
  lastCheckedMinute = m;

  const int jadwalJam[] = {7, 12, 14, 15, 17};
  const int jadwalMenit[] = {0, 0, 0, 30, 0};
  const int jadwalCount = 5;

  for (int i = 0; i < jadwalCount; i++) {
    if (h == jadwalJam[i] && m == jadwalMenit[i] && !alreadyFed[i]) {
      String waktuStr = String(jadwalJam[i]) + ":" + (jadwalMenit[i] < 10 ? "0" : "") + String(jadwalMenit[i]);
      openFeeding("Otomatis " + waktuStr);
      alreadyFed[i] = true;
      char fedTopic[20];
      snprintf(fedTopic, sizeof(fedTopic), "%s%d", MQTT_TOPIC_FED_PREFIX, i);
      client.publish(fedTopic, "true");
    }
  }
}

// --- Baca jarak ---
long readUltrasonicDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return (duration == 0) ? -1 : duration * 0.034 / 2;
}

// --- Fungsi callback MQTT ---
void callback(char* topic, byte* payload, unsigned int length) {
  String messageTemp;
  for (int i = 0; i < length; i++) {
    messageTemp += (char)payload[i];
  }

  Serial.print("Pesan diterima [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(messageTemp);

  if (String(topic) == MQTT_TOPIC_COMMAND) {
    if (messageTemp == "/berikan_pakan_ikan") {
      openFeeding("Manual via Web");
    }
  }
}

// --- Koneksi WiFi ---
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi connection failed.");
  }
}

// --- Koneksi MQTT ---
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Mencoba koneksi ke MQTT...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("terhubung");
      client.subscribe(MQTT_TOPIC_COMMAND);
    } else {
      Serial.print("gagal, rc=");
      Serial.print(client.state());
      Serial.println(" coba lagi dalam 5 detik");
      delay(5000);
    }
  }
}

// --- Setup utama ---
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  servo.attach(SERVO_PIN);
  servo.write(0);

  Wire.begin(4, 16);  // SDA = GPIO 4, SCL = GPIO 16

  if (!rtc.begin()) {
    Serial.println("RTC tidak ditemukan!");
    while (1) delay(10);
  }

  connectWiFi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); // <- Tambahkan ini
  espClient.setInsecure();
}

// --- Loop utama ---
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      reconnectMQTT();
    }
    client.loop();

    if (millis() - mqtt_lasttime > MQTT_MTBS) {
      if (client.connected()) {
        char buffer[10];
        String servoStatus = servoIsOpen ? "Terbuka" : "Tertutup";
        String relayStatus = digitalRead(RELAY_PIN) == RELAY_ON ? "ON" : "OFF";
        String timeStr = getTimeString();
        long distance = readUltrasonicDistance();
        String feedStatus;

        // Logika untuk menentukan status pakan
        if (distance >= 52 && distance <= 55) {
          feedStatus = "Pakan masih ada";
        } else if (distance > 55) {
          feedStatus = "Pakan sudah mau habis";
        } else {
          feedStatus = "Pakan tidak terdeteksi";
        }

        client.publish(MQTT_TOPIC_TIME, timeStr.c_str());
        client.publish(MQTT_TOPIC_SERVO, servoStatus.c_str());
        client.publish(MQTT_TOPIC_RELAY, relayStatus.c_str());
        client.publish(MQTT_TOPIC_FEED_STATUS, feedStatus.c_str()); // Publikasi status pakan

        for (int i = 0; i < 5; i++) {
          char fedTopic[20];
          snprintf(fedTopic, sizeof(fedTopic), "%s%d", MQTT_TOPIC_FED_PREFIX, i);
          client.publish(fedTopic, alreadyFed[i] ? "true" : "false");
        }
        client.publish(MQTT_TOPIC_LOG, feedingLog.c_str());
      }
      mqtt_lasttime = millis();
    }
  }

  resetDailySchedule();
  checkAutoFeeding();

  if (servoIsOpen && millis() - servoOpenTime > SERVO_DURATION) {
    servo.write(0);
    digitalWrite(RELAY_PIN, RELAY_OFF);
    servoIsOpen = false;
    client.publish(MQTT_TOPIC_SERVO, "Tertutup");
    client.publish(MQTT_TOPIC_RELAY, "OFF");
  }
}
