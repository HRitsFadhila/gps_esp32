#include <WiFi.h>
#include <WebServer.h>
#include <TinyGPSPlus.h>
#include <Firebase_ESP_Client.h>

// GPS UART config
#define RXD2 16
#define TXD2 17
#define GPSBaud 115200

// Relay pin
#define RELAY_PIN 14

// WiFi credentials
const char* apSSID = "ESP32_Hotspot";
const char* apPassword = "12345678";
const char* staSSID = "GPS2";
const char* staPassword = "1234567890";

// Firebase config
#define API_KEY "AIzaSyCprGyIVbsc-Yv3zGvhPSpH0uqiX3OVcbc"
#define DATABASE_URL "https://gps-project-a5c9a-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL "rits.fadhila@gmail.com"
#define USER_PASSWORD "1234567890"

TinyGPSPlus gps;
WebServer server(80);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

bool currentRelayStatus = false;
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 10000;
String deviceId;

double lastValidLat = 0.0;
double lastValidLng = 0.0;
String lastValidTime = "00:00:00";
String lastValidDate = "1970-01-01";

// === Setup ===
void setup() {
  Serial.begin(115200);
  Serial2.begin(GPSBaud, SERIAL_8N1, RXD2, TXD2);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  setupWiFi();
  deviceId = getDeviceIdFromMAC();
  Serial.println("🆔 Device ID: " + deviceId);

  setupFirebase();
  setupWebRoutes();
  server.begin();
}

// === Main Loop ===
void loop() {
  while (Serial2.available()) gps.encode(Serial2.read());

  if (millis() - lastSendTime >= sendInterval) {
    lastSendTime = millis();
    uploadGPSData();
    syncRelayState();
  }

  server.handleClient();
}

// === Helper Functions ===

String getDeviceIdFromMAC() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(staSSID, staPassword);
  Serial.print("📶 Connecting");

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(500);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n⚠ WiFi failed. Starting hotspot...");
  }

  WiFi.softAP(apSSID, apPassword);
  Serial.println("📡 Hotspot IP: " + WiFi.softAPIP().toString());
}

void setupFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  String path = "/devices/" + deviceId + "/relay";
  if (!Firebase.RTDB.getBool(&fbdo, path)) {
    Firebase.RTDB.setBool(&fbdo, path, false);
    Serial.println("🛠 Initializing relay state to false");
  }

  currentRelayStatus = fbdo.boolData();
  digitalWrite(RELAY_PIN, currentRelayStatus ? HIGH : LOW);
}

void uploadGPSData() {
  // if (!gps.location.isValid()) {
  //   Serial.println("❌ Invalid GPS");
  //   Serial.println("Satellites: " + String(gps.satellites.value()));
  //   return;
  // }

  FirebaseJson json;
  int satelit = gps.satellites.isValid() ? gps.satellites.value() : 0;
  json.set("satellites", satelit);

    if (gps.location.isValid()) {
    lastValidLat = gps.location.lat();
    lastValidLng = gps.location.lng();
    json.set("latitude", lastValidLat);
    json.set("longitude", lastValidLng);
  }

  // Simpan waktu terakhir yang valid
  if (gps.time.isValid()) {
    int hUTC = gps.time.hour();
    int hWITA = (hUTC + 8) % 24;

    char utcTime[9], witaTime[9];
    sprintf(utcTime, "%02d:%02d:%02d", hUTC, gps.time.minute(), gps.time.second());
    sprintf(witaTime, "%02d:%02d:%02d", hWITA, gps.time.minute(), gps.time.second());

    json.set("utc_time", String(utcTime));
    json.set("wita_time", String(witaTime));
  }

   // Simpan tanggal terakhir yang valid
  if (gps.date.isValid()) {
    char date[11];
    sprintf(date, "%04d-%02d-%02d", gps.date.year(), gps.date.month(), gps.date.day());
    lastValidDate = String(date);
    json.set("date", lastValidDate);
  }
  
  if (gps.speed.isValid()) json.set("speed_kmph", gps.speed.kmph());
  if (gps.course.isValid()) json.set("course_deg", gps.course.deg());
  if (gps.altitude.isValid()) json.set("altitude_m", gps.altitude.meters());
  if (gps.hdop.isValid()) json.set("hdop", gps.hdop.hdop());

  String path = "/devices/" + deviceId + "/gps";
  if (Firebase.RTDB.updateNode(&fbdo, path, &json)) {
    Serial.println("✅ GPS data uploaded");
  } else {
    Serial.println("❌ Upload failed: " + fbdo.errorReason());
  }
}

void syncRelayState() {
  String path = "/devices/" + deviceId + "/relay";
  if (Firebase.RTDB.getBool(&fbdo, path)) {
    if (fbdo.dataType() == "boolean") {
      bool status = fbdo.boolData();
      if (status != currentRelayStatus) {
        currentRelayStatus = status;
        digitalWrite(RELAY_PIN, status ? HIGH : LOW);
        Serial.println("🔁 Relay: " + String(status ? "ON" : "OFF"));
      }
    }
  } else {
    Serial.println("❌ Failed to read relay: " + fbdo.errorReason());
  }
}

void setupWebRoutes() {
  server.on("/", handleRoot);

  server.on("/relay/on", []() {
    updateRelay(true);
  });

  server.on("/relay/off", []() {
    updateRelay(false);
  });
}

void updateRelay(bool status) {
  currentRelayStatus = status;
  digitalWrite(RELAY_PIN, status ? HIGH : LOW);

  String path = "/devices/" + deviceId + "/relay";
  Firebase.RTDB.setBool(&fbdo, path, status);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  String html = "<html><head><title>GPS Tracker</title></head><body>";
  html += "<h1>GPS Status</h1>";
  html += "<p><strong>Device ID:</strong> " + deviceId + "</p>";
  html += "<p><strong>Latitude:</strong> " + (gps.location.isValid() ? String(gps.location.lat(), 6) : "INVALID") + "</p>";
  html += "<p><strong>Longitude:</strong> " + (gps.location.isValid() ? String(gps.location.lng(), 6) : "INVALID") + "</p>";
  html += "<p><strong>Date:</strong> " + (gps.date.isValid() ? String(gps.date.year()) + "-" + String(gps.date.month()) + "-" + String(gps.date.day()) : "INVALID") + "</p>";
  html += "<p><strong>Time (WITA):</strong> " + String((gps.time.isValid() ? (gps.time.hour() + 8) % 24 : 0)) + ":" + String(gps.time.minute()) + ":" + String(gps.time.second()) + "</p>";
  html += "<p><strong>Satellites:</strong> " + String(gps.satellites.value()) + "</p>";
  html += "<p><strong>Speed:</strong> " + String(gps.speed.kmph()) + " km/h</p>";
  html += "<p><strong>Direction:</strong> " + String(gps.course.deg()) + "°</p>";
  html += "<p><strong>Altitude:</strong> " + String(gps.altitude.meters()) + " m</p>";
  html += "<p><strong>HDOP:</strong> " + String(gps.hdop.hdop()) + "</p>";
  html += "<p><strong>Relay Status:</strong> " + String(currentRelayStatus ? "ON" : "OFF") + "</p>";
  html += "<form action=\"/relay/on\" method=\"POST\"><button type=\"submit\">Relay ON</button></form>";
  html += "<form action=\"/relay/off\" method=\"POST\"><button type=\"submit\">Relay OFF</button></form>";

  if (gps.location.isValid()) {
    html += "<p><a href=\"https://www.google.com/maps/search/?api=1&query=" +
            String(gps.location.lat(), 6) + "," +
            String(gps.location.lng(), 6) +
            "\" target=\"_blank\">📍 Open in Google Maps</a></p>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}
