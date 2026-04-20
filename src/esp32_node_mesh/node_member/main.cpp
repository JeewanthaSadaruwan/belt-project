/*
 * ============================================================
 *  ESP32 MESH NODE — Belt Bridge Node (with LoRa fallback)
 *
 *  ROLE IN SYSTEM:
 *    Sits physically next to the Belt ESP32, connected by wire.
 *    Receives real sensor data from Belt ESP32 over UART,
 *    then forwards it over painlessMesh (ESP-NOW) and optionally
 *    also over LoRa as a parallel/redundant transport.
 *
 *  TRANSPORT DECISION FLAG:
 *    USE_ESP_NOW_ONLY true  → mesh/ESP-NOW only, LoRa is silent
 *    USE_ESP_NOW_ONLY false → mesh AND LoRa both transmit
 *
 *  WIRING (Belt ESP32 → This Node):
 *    Belt GPIO4 (TX1)  ──────►  This Node GPIO16 (RX2)
 *    Belt GND          ──────►  This Node GND
 *
 *  LORA MODULE WIRING (SX1278 / Ra-02):
 *    NSS/CS   → GPIO 5
 *    RST      → GPIO 14
 *    DIO0     → GPIO 26
 *    SCK      → GPIO 18
 *    MISO     → GPIO 19
 *    MOSI     → GPIO 23
 *    3.3V / GND
 *
 *  OTHER HARDWARE:
 *    Belt UART RX    GPIO16 (Serial2 RX)
 *    Panic Button    GPIO4  (active LOW, internal pull-up)
 *    Status LED      GPIO2
 *
 *  MESH CONFIG:
 *    SSID     : ESP_MESH
 *    Password : 12345678
 *    Port     : 5555
 *
 *  LORA CONFIG:
 *    Frequency : 433 MHz
 *    SF        : 7    BW: 125 kHz    CR: 4/5
 *    SyncWord  : 0x12   CRC: enabled   TxPower: 20 dBm
 * ============================================================
 */

#include "painlessMesh.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <LoRa.h>

// ── Transport decision ────────────────────────────────────────
// true  → ESP-NOW/mesh only, LoRa stays silent
// false → both mesh AND LoRa transmit simultaneously
#define USE_ESP_NOW_ONLY true

// ── Mesh config ───────────────────────────────────────────────
#define MESH_PREFIX   "ESP_MESH"
#define MESH_PASSWORD "12345678"
#define MESH_PORT     5555

// ── Wired UART from Belt ESP32 ────────────────────────────────
#define BELT_UART_RX   16
#define BELT_UART_TX   17    // not used (one-way receive only)
#define BELT_UART_BAUD 115200

// ── Panic button ──────────────────────────────────────────────
#define PANIC_BUTTON   4

// ── LoRa pins (same as example2) ─────────────────────────────
#define LORA_CS        5
#define LORA_RST       14
#define LORA_DIO0      26
#define LORA_SCK       18
#define LORA_MISO      19
#define LORA_MOSI      23
#define LORA_FREQUENCY 433E6
#define LED_PIN        2

#define LORA_PKT_DATA  0x10
#define LORA_MAX_DATA  220

static uint16_t loraPacketCounter = 0;
static bool     loraReady         = false;

// ── Mesh objects ──────────────────────────────────────────────
Scheduler userScheduler;
painlessMesh mesh;

String nodeName = "TeamMember2";
bool   meshReady = false;

// ── Last data received from Belt ESP32 via UART ───────────────
struct BeltData {
  double lat     = 0.0;
  double lon     = 0.0;
  float  heading = 0.0f;
  float  dist    = 0.0f;
  float  rel     = 0.0f;
  float  temp    = 0.0f;
  int    sats    = 0;
  bool   gpsOk   = false;
  bool   fresh   = false;
} beltData;


// ════════════════════════════════════════════════════════════
//  LED HELPER
// ════════════════════════════════════════════════════════════
static void blinkLED(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(onMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}


// ════════════════════════════════════════════════════════════
//  LORA TX (from example2 — no modifications)
// ════════════════════════════════════════════════════════════
static bool initLoRa() {
  Serial.println("[LoRa] Starting LoRa transmitter...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] Init failed!");
    return false;
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  LoRa.setTxPower(20);

  Serial.printf("[LoRa] Frequency: %.0f Hz\n", (float)LORA_FREQUENCY);
  Serial.println("[LoRa] Init success!");
  return true;
}

static bool sendLoRaPacketRaw(uint8_t type, const char* data) {
  uint8_t buffer[4 + LORA_MAX_DATA];
  size_t dataLen = strlen(data);
  if (dataLen > LORA_MAX_DATA) dataLen = LORA_MAX_DATA;

  buffer[0] = type;
  buffer[1] = (uint8_t)(loraPacketCounter >> 8);
  buffer[2] = (uint8_t)(loraPacketCounter & 0xFF);
  buffer[3] = (uint8_t)dataLen;
  memcpy(&buffer[4], data, dataLen);

  LoRa.beginPacket();
  LoRa.write(buffer, 4 + dataLen);
  int result = LoRa.endPacket();
  if (result) loraPacketCounter++;
  return result != 0;
}


// ════════════════════════════════════════════════════════════
//  BELT UART RECEIVE
// ════════════════════════════════════════════════════════════
void readBeltUART() {
  static String buf = "";
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        beltData.lat     = doc["lat"]     | 0.0;
        beltData.lon     = doc["lon"]     | 0.0;
        beltData.heading = doc["heading"] | 0.0f;
        beltData.dist    = doc["dist"]    | 0.0f;
        beltData.rel     = doc["rel"]     | 0.0f;
        beltData.temp    = doc["temp"]    | 0.0f;
        beltData.sats    = doc["sats"]    | 0;
        beltData.gpsOk   = (doc["gps"]   | 0) == 1;
        beltData.fresh   = true;
        Serial.println("[BELT] RX: " + buf);
      } else {
        Serial.println("[BELT] JSON parse error: " + buf);
      }
      buf = "";
    } else {
      buf += c;
      if (buf.length() > 300) buf = "";  // overflow guard
    }
  }
}


// ════════════════════════════════════════════════════════════
//  SEND — mesh broadcast + optional LoRa TX
// ════════════════════════════════════════════════════════════
void sendMessage() {
  if (!meshReady) return;

  int rssi  = WiFi.RSSI();
  int panic = (digitalRead(PANIC_BUTTON) == LOW) ? 1 : 0;

  // Closest upstream mesh node
  uint32_t parentId = 0;
  SimpleList<uint32_t> list = mesh.getNodeList();
  if (list.size() > 0) parentId = list.front();

  if (!beltData.fresh) {
    Serial.println("[WARN] No new belt data since last broadcast — using last known values");
  }
  beltData.fresh = false;

  // ── 1. Always send over painlessMesh (ESP-NOW) ─────────────
  String meshMsg = nodeName +
    " Parent:"  + String(parentId) +
    " Temp:"    + String(beltData.temp, 1) +
    " CO:0 CO2:0 Fire:0 Heart:0" +
    " Lat:"     + String(beltData.lat, 6) +
    " Lon:"     + String(beltData.lon, 6) +
    " Heading:" + String(beltData.heading, 1) +
    " Dist:"    + String(beltData.dist, 1) +
    " Sats:"    + String(beltData.sats) +
    " GPS:"     + String(beltData.gpsOk ? 1 : 0) +
    " Image:0"  +
    " Panic:"   + String(panic) +
    " RSSI:"    + String(rssi);

  mesh.sendBroadcast(meshMsg);
  Serial.println("[MESH TX] " + meshMsg);

  // ── 2. LoRa TX — only when USE_ESP_NOW_ONLY is false ───────
#if !USE_ESP_NOW_ONLY
  if (loraReady) {
    char loraPayload[LORA_MAX_DATA];
    snprintf(loraPayload, sizeof(loraPayload),
      "name=%s,lat=%.6f,lon=%.6f,hdg=%.1f,dist=%.1f,rel=%.1f,"
      "temp=%.1f,sats=%d,gps=%d,panic=%d,rssi=%d",
      nodeName.c_str(),
      beltData.lat, beltData.lon,
      beltData.heading, beltData.dist, beltData.rel,
      beltData.temp, beltData.sats,
      beltData.gpsOk ? 1 : 0,
      panic, rssi);

    bool ok = sendLoRaPacketRaw(LORA_PKT_DATA, loraPayload);
    if (ok) {
      blinkLED(1, 80, 0);
      Serial.printf("[LoRa TX] #%04u  OK  payload: %s\n", loraPacketCounter - 1, loraPayload);
    } else {
      blinkLED(3, 80, 80);
      Serial.printf("[LoRa TX] #%04u  FAIL\n", loraPacketCounter);
    }
  }
#endif
}

Task taskSend(5000, TASK_FOREVER, &sendMessage);

void newConnectionCallback(uint32_t nodeId) {
  meshReady = true;
  Serial.printf("[MESH] Connected: node %u\n", nodeId);
}


// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(PANIC_BUTTON, INPUT_PULLUP);

  // Wired UART from Belt ESP32
  Serial2.begin(BELT_UART_BAUD, SERIAL_8N1, BELT_UART_RX, BELT_UART_TX);
  Serial.println("[BELT] Serial2 RX ready on GPIO16 ← Belt ESP32 GPIO4");

  // LoRa — init only when needed
#if !USE_ESP_NOW_ONLY
  loraReady = initLoRa();
  if (!loraReady) Serial.println("[LoRa] DISABLED — init failed, LoRa TX will be skipped");
#else
  Serial.println("[LoRa] Disabled by USE_ESP_NOW_ONLY flag — mesh only");
#endif

  // Mesh
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onNewConnection(&newConnectionCallback);

  userScheduler.addTask(taskSend);
  taskSend.enable();

  Serial.printf("[MESH] Joining mesh: %s  (USE_ESP_NOW_ONLY=%s)\n",
    MESH_PREFIX, USE_ESP_NOW_ONLY ? "true" : "false");
}


// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  readBeltUART();   // drain UART buffer continuously
  mesh.update();
}
