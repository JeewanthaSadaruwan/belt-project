/*
 * BASE STATION FIRMWARE (Consolidated)
 * Receives sensor data from belt device via LoRa
 * Provides WiFi AP and WebSocket/REST API for frontend dashboard
 * 
 * Single-file Arduino-style code - no separate headers needed
 */

#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>

// =====================================================
//  PIN DEFINITIONS
// =====================================================

// LoRa SPI Pins (RA-02 SX1278)
#define LORA_CS     5
#define LORA_RST    14
#define LORA_DIO0   26
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23

// Status LED
#define STATUS_LED  2

// =====================================================
//  WiFi AP CONFIGURATION
// =====================================================
#define AP_SSID              "BeltStation"
#define AP_PASSWORD          "belt12345"

// =====================================================
//  LoRa CONFIGURATION
// =====================================================
#define LORA_FREQUENCY      433E6

// =====================================================
//  PROTOCOL DEFINITIONS
// =====================================================

// Packet types
#define PKT_DATA            0x10
#define PKT_STATUS          0x11

// Protocol constants
#define MAX_PACKET_SIZE     255
#define MAX_DATA_SIZE       240

// =====================================================
//  DATA STRUCTURES
// =====================================================

// LoRa packet structure
struct LoRaPacket {
  uint8_t type;
  uint16_t counter;
  uint8_t dataLen;
  char data[240];
};

// =====================================================
//  GLOBAL VARIABLES
// =====================================================

// Web Server & WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Data storage
String latestDataStr = "{}";
std::vector<String> dataHistory;
const int MAX_HISTORY = 50;

// Statistics
unsigned long startTime = 0;
int packetsReceived = 0;
uint16_t loraPacketCounter = 0;

// =====================================================
//  SIMPLE LANDING PAGE HTML
// =====================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Belt Base Station</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background: linear-gradient(135deg, #667eea, #764ba2);
            color: white;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            margin: 0;
        }
        .container {
            text-align: center;
            background: rgba(255, 255, 255, 0.1);
            padding: 40px;
            border-radius: 15px;
            backdrop-filter: blur(10px);
            max-width: 600px;
        }
        h1 { margin: 0 0 20px 0; }
        .info { margin: 20px 0; font-size: 1.1em; }
        .api-list {
            background: rgba(0, 0, 0, 0.2);
            padding: 20px;
            border-radius: 10px;
            margin-top: 20px;
            text-align: left;
        }
        .api-item { margin: 10px 0; }
        a { color: #fff; text-decoration: underline; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎯 Belt Base Station</h1>
        <div class="info">✅ WebSocket & API Server Running</div>
        <div class="info">📡 Receiving LoRa data from belt device</div>
        <div class="api-list">
            <strong>API Endpoints:</strong>
            <div class="api-item">📊 <a href="/api/data">/api/data</a> - Latest sensor data</div>
            <div class="api-item">📈 <a href="/api/history">/api/history</a> - Historical data</div>
            <div class="api-item">ℹ️ <a href="/api/info">/api/info</a> - System info</div>
            <div class="api-item">🔌 ws://192.168.4.1/ws - WebSocket stream</div>
        </div>
        <div class="info" style="margin-top: 30px;">
            💡 <strong>To view the dashboard:</strong><br>
            Open the frontend web application in your browser
        </div>
    </div>
</body>
</html>
)rawliteral";

// =====================================================
//  FORWARD DECLARATIONS
// =====================================================
bool initLoRa();
void startReceive();
bool receiveLoRaPacket(LoRaPacket* packet);
void handleLoRaPacket(LoRaPacket* packet);
void printSensorData(const char* data);
void initWiFiAP();
void initWebServer();
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len);
void addToHistory(const String& data);

// =====================================================
//  LoRa FUNCTIONS
// =====================================================

bool initLoRa() {
  Serial.println("[LoRa] Starting LoRa receiver...");
  Serial.println("[LoRa] Pin Configuration:");
  Serial.print("  CS:   GPIO "); Serial.println(LORA_CS);
  Serial.print("  RST:  GPIO "); Serial.println(LORA_RST);
  Serial.print("  DIO0: GPIO "); Serial.println(LORA_DIO0);
  Serial.print("  SCK:  GPIO "); Serial.println(LORA_SCK);
  Serial.print("  MISO: GPIO "); Serial.println(LORA_MISO);
  Serial.print("  MOSI: GPIO "); Serial.println(LORA_MOSI);
  
  Serial.println("[LoRa] Initializing SPI...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  delay(100);
  
  Serial.println("[LoRa] Setting LoRa pins...");
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  delay(100);

  Serial.print("[LoRa] Attempting to start at ");
  Serial.print((long)LORA_FREQUENCY);
  Serial.println(" Hz...");
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("\n[LoRa] *** INIT FAILED ***");
    Serial.println("[LoRa] Check connections!");
    return false;
  }

  // Configure LoRa (matching belt device)
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  
  Serial.print("[LoRa] ✅ Frequency: ");
  Serial.print((long)LORA_FREQUENCY);
  Serial.println(" Hz");
  Serial.println("[LoRa] Receiver ready!");
  return true;
}

void startReceive() {
  LoRa.receive();
}

bool receiveLoRaPacket(LoRaPacket* packet) {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) {
    return false;
  }
  
  // Read packet header
  packet->type = LoRa.read();
  packet->counter = (LoRa.read() << 8) | LoRa.read();
  packet->dataLen = LoRa.read();
  
  // Read data payload
  int idx = 0;
  while (LoRa.available() && idx < packet->dataLen && idx < MAX_DATA_SIZE) {
    packet->data[idx++] = LoRa.read();
  }
  packet->data[idx] = '\0';
  
  // Get signal quality
  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  
  Serial.print("\n[LoRa RX] ");
  Serial.print("Packet #");
  Serial.print(packet->counter);
  Serial.print(" | Type: 0x");
  Serial.print(packet->type, HEX);
  Serial.print(" | Size: ");
  Serial.print(packet->dataLen);
  Serial.print(" bytes");
  Serial.print(" | RSSI: ");
  Serial.print(rssi);
  Serial.print(" dBm | SNR: ");
  Serial.print(snr);
  Serial.println(" dB");
  
  return true;
}

// =====================================================
//  WiFi AP FUNCTIONS
// =====================================================

void initWiFiAP() {
  Serial.println("\n[WiFi] Setting up Access Point...");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  delay(1000);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] ✅ Access Point IP: ");
  Serial.println(IP);
  Serial.printf("[WiFi] SSID: %s\n", AP_SSID);
  Serial.printf("[WiFi] Password: %s\n", AP_PASSWORD);
  Serial.println("[WiFi] No internet connection required!");
}

// =====================================================
//  WEB SERVER FUNCTIONS
// =====================================================

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WebSocket] Client #%u connected\n", client->id());
    // Send latest data to newly connected client
    if (latestDataStr.length() > 2) {
      client->text(latestDataStr);
    }
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WebSocket] Client #%u disconnected\n", client->id());
  }
}

void initWebServer() {
  Serial.println("\n[WebServer] Setting up routes...");
  
  // WebSocket handler
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  
  // Root page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  
  // API: Get latest data
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", latestDataStr);
  });
  
  // API: Get data history
  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request) {
    String historyJson = "[";
    for (size_t i = 0; i < dataHistory.size(); i++) {
      historyJson += dataHistory[i];
      if (i < dataHistory.size() - 1) historyJson += ",";
    }
    historyJson += "]";
    request->send(200, "application/json", historyJson);
  });
  
  // API: Get system info
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["uptime"] = millis() - startTime;
    doc["packets_received"] = packetsReceived;
    doc["ssid"] = AP_SSID;
    doc["ip"] = WiFi.softAPIP().toString();
    doc["clients_connected"] = WiFi.softAPgetStationNum();
    doc["ws_clients"] = ws.count();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // Enable CORS
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  
  server.begin();
  Serial.println("[WebServer] ✅ Server started");
  Serial.println("[WebServer] Available endpoints:");
  Serial.println("  - http://192.168.4.1/");
  Serial.println("  - http://192.168.4.1/api/data");
  Serial.println("  - http://192.168.4.1/api/history");
  Serial.println("  - http://192.168.4.1/api/info");
  Serial.println("  - ws://192.168.4.1/ws");
}

void addToHistory(const String& data) {
  dataHistory.push_back(data);
  if (dataHistory.size() > MAX_HISTORY) {
    dataHistory.erase(dataHistory.begin());
  }
}

// =====================================================
//  PACKET HANDLING
// =====================================================

void handleLoRaPacket(LoRaPacket* packet) {
  switch (packet->type) {
    case PKT_DATA:
      Serial.println("\n========== SENSOR DATA RECEIVED ==========");
      printSensorData(packet->data);
      Serial.println("==========================================\n");
      
      packetsReceived++;
      Serial.print("[Statistics] Total packets received: ");
      Serial.println(packetsReceived);
      
      // Parse JSON and add metadata
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, packet->data);
      
      if (!error) {
        // Add signal quality
        doc["rssi"] = LoRa.packetRssi();
        doc["snr"] = LoRa.packetSnr();
        doc["uptime"] = millis() - startTime;
        doc["timestamp"] = millis();
        
        // Convert to string
        latestDataStr = "";
        serializeJson(doc, latestDataStr);
        
        // Add to history
        addToHistory(latestDataStr);
        
        // Broadcast to all WebSocket clients
        ws.textAll(latestDataStr);
        
        Serial.println("[WebSocket] Data broadcasted to all clients");
      } else {
        Serial.print("[ERROR] JSON parse failed: ");
        Serial.println(error.c_str());
        
        // Still store raw data
        latestDataStr = packet->data;
      }
      
      // Blink LED
      digitalWrite(STATUS_LED, HIGH);
      delay(50);
      digitalWrite(STATUS_LED, LOW);
      break;
      
    case PKT_STATUS:
      Serial.print("[Status] ");
      Serial.println(packet->data);
      break;
      
    default:
      Serial.print("[Unknown] Type 0x");
      Serial.print(packet->type, HEX);
      Serial.print(": ");
      Serial.println(packet->data);
  }
}

void printSensorData(const char* data) {
  // Parse JSON data for pretty printing
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, data);
  
  if (error) {
    Serial.print("Raw Data: ");
    Serial.println(data);
    return;
  }
  
  // GPS Data
  Serial.println("📍 GPS:");
  if (doc["gps"]["valid"]) {
    Serial.print("  Latitude:  ");
    Serial.println(doc["gps"]["lat"].as<double>(), 6);
    Serial.print("  Longitude: ");
    Serial.println(doc["gps"]["lng"].as<double>(), 6);
    Serial.print("  Altitude:  ");
    Serial.print(doc["gps"]["alt"].as<float>(), 1);
    Serial.println(" m");
    Serial.print("  Speed:     ");
    Serial.print(doc["gps"]["speed"].as<float>(), 1);
    Serial.println(" km/h");
    Serial.print("  Satellites: ");
    Serial.println(doc["gps"]["sats"].as<int>());
  } else {
    Serial.println("  No GPS fix");
  }
  
  // IMU Data
  Serial.println("\n🔄 IMU:");
  if (doc["imu"]["valid"]) {
    Serial.print("  Acceleration: X=");
    Serial.print(doc["imu"]["accel"]["x"].as<float>(), 2);
    Serial.print(" Y=");
    Serial.print(doc["imu"]["accel"]["y"].as<float>(), 2);
    Serial.print(" Z=");
    Serial.print(doc["imu"]["accel"]["z"].as<float>(), 2);
    Serial.println(" m/s²");
    
    Serial.print("  Gyroscope:    X=");
    Serial.print(doc["imu"]["gyro"]["x"].as<float>(), 2);
    Serial.print(" Y=");
    Serial.print(doc["imu"]["gyro"]["y"].as<float>(), 2);
    Serial.print(" Z=");
    Serial.print(doc["imu"]["gyro"]["z"].as<float>(), 2);
    Serial.println(" rad/s");
    
    Serial.print("  Temperature:  ");
    Serial.print(doc["imu"]["temp"].as<float>(), 1);
    Serial.println(" °C");
  } else {
    Serial.println("  IMU not available");
  }
  
  // Motor Data
  Serial.println("\n⚙️ Motors:");
  Serial.print("  Left:  Speed=");
  Serial.print(doc["motors"]["left_speed"].as<int>());
  Serial.print(" Dir=");
  Serial.println(doc["motors"]["left_dir"].as<const char*>());
  Serial.print("  Right: Speed=");
  Serial.print(doc["motors"]["right_speed"].as<int>());
  Serial.print(" Dir=");
  Serial.println(doc["motors"]["right_dir"].as<const char*>());
}

// =====================================================
//  ARDUINO MAIN FUNCTIONS
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n====================================");
  Serial.println("   BASE STATION - LoRa Receiver");
  Serial.println("   WiFi AP + WebSocket + REST API");
  Serial.println("====================================");
  
  // Status LED
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  
  startTime = millis();
  
  // Initialize WiFi AP
  initWiFiAP();
  
  // Initialize LoRa
  if (!initLoRa()) {
    Serial.println("[FATAL] LoRa initialization failed!");
    while (1) {
      digitalWrite(STATUS_LED, HIGH);
      delay(500);
      digitalWrite(STATUS_LED, LOW);
      delay(500);
    }
  }
  
  startReceive();
  
  // Initialize Web Server
  initWebServer();
  
  Serial.println("\n✅ Base Station Ready!");
  Serial.println("📡 Listening for belt device...");
  Serial.println("💻 Dashboard available at http://192.168.4.1\n");
  
  digitalWrite(STATUS_LED, LOW);
}

void loop() {
  // Handle incoming LoRa packets
  LoRaPacket packet;
  if (receiveLoRaPacket(&packet)) {
    handleLoRaPacket(&packet);
  }
  
  // Clean up WebSocket clients
  ws.cleanupClients();
  
  delay(10);
}
