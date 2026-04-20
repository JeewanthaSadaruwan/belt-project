/*
 * ============================================================
 *  ESP32 MESH BASE STATION — Dashboard, Mesh Root & LoRa RX
 *
 *  ROLE IN SYSTEM:
 *    Root of the painlessMesh network. Receives broadcasts from
 *    all mesh nodes, parses sensor data, and serves a live HTML
 *    dashboard over its own WiFi softAP.
 *    When USE_ESP_NOW_ONLY is false, also listens on LoRa and
 *    prints/logs received LoRa packets alongside mesh data.
 *
 *  TRANSPORT DECISION FLAG:
 *    USE_ESP_NOW_ONLY true  → mesh only, LoRa RX not initialised
 *    USE_ESP_NOW_ONLY false → mesh + LoRa RX both active
 *    !! Must match the flag in node_member/main.cpp !!
 *
 *  DASHBOARD ACCESS:
 *    Connect to WiFi "ESP32_DASH" (pw: 12345678)
 *    Open browser → http://192.168.4.1/
 *    Page auto-refreshes every 2 seconds.
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
 *  MESH CONFIG:
 *    SSID     : ESP_MESH   Password : 12345678   Port : 5555
 *
 *  LORA CONFIG:
 *    Frequency : 433 MHz   SF: 7   BW: 125 kHz   CR: 4/5
 *    SyncWord  : 0x12      CRC: enabled
 * ============================================================
 */

#include "painlessMesh.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>

// ── Transport decision ────────────────────────────────────────
// Must match node_member/main.cpp
// true  → mesh only, LoRa RX stays uninitialised
// false → mesh + LoRa RX both active
#define USE_ESP_NOW_ONLY true

// ── Mesh config ───────────────────────────────────────────────
#define MESH_PREFIX   "ESP_MESH"
#define MESH_PASSWORD "12345678"
#define MESH_PORT     5555

// ── LoRa pins (same as example3) ─────────────────────────────
#define LORA_CS        5
#define LORA_RST       14
#define LORA_DIO0      26
#define LORA_SCK       18
#define LORA_MISO      19
#define LORA_MOSI      23
#define LORA_FREQUENCY 433E6
#define LED_PIN        2

#define LORA_MAX_DATA  240
#define LORA_RING_SIZE 4

static bool loraReady = false;

Scheduler  userScheduler;
painlessMesh mesh;
WebServer  server(80);


// ════════════════════════════════════════════════════════════
//  LORA RX RING BUFFER (from example3 — no modifications)
// ════════════════════════════════════════════════════════════
struct LoRaRxEntry {
  char     data[LORA_MAX_DATA];
  uint8_t  pktType;
  uint16_t pktCounter;
  int      rssi;
  bool     valid;
};
volatile LoRaRxEntry loraRing[LORA_RING_SIZE];
volatile int loraRingHead = 0;
volatile int loraRingTail = 0;

// No IRAM_ATTR — LoRa.read() uses SPI which lives in flash.
// IRAM_ATTR here causes Guru Meditation (LoadProhibited) crash.
void onLoRaReceive(int packetSize) {
  if (packetSize < 4) return;

  int nextHead = (loraRingHead + 1) % LORA_RING_SIZE;
  if (nextHead == loraRingTail) return;  // ring full, drop

  volatile LoRaRxEntry* entry = &loraRing[loraRingHead];
  entry->rssi       = LoRa.packetRssi();
  entry->pktType    = LoRa.read();
  entry->pktCounter = ((uint16_t)LoRa.read() << 8) | LoRa.read();
  uint8_t dataLen   = LoRa.read();
  if (dataLen > LORA_MAX_DATA - 1) dataLen = LORA_MAX_DATA - 1;

  int idx = 0;
  while (LoRa.available() && idx < dataLen)
    entry->data[idx++] = (char)LoRa.read();
  entry->data[idx] = '\0';
  entry->valid = true;

  loraRingHead = nextHead;
}

static void blinkLED(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(onMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

bool initLoRaRX() {
  Serial.println("[LoRa] Starting LoRa receiver...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  delay(100);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  delay(100);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] Init failed!");
    return false;
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();

  LoRa.onReceive(onLoRaReceive);
  LoRa.receive();  // continuous RX mode

  Serial.printf("[LoRa] Frequency: %.0f Hz — RX_CONTINUOUS active\n", (float)LORA_FREQUENCY);
  Serial.println("[LoRa] Init success!");
  return true;
}

// Drain ring buffer and print any received LoRa packets
void processLoRaPackets() {
  while (loraRingTail != loraRingHead) {
    volatile LoRaRxEntry* entry = &loraRing[loraRingTail];
    loraRingTail = (loraRingTail + 1) % LORA_RING_SIZE;
    if (!entry->valid) continue;

    blinkLED(2, 80, 80);

    Serial.println("\n════════════════════════════════════════");
    Serial.printf ("  [LoRa RX] PACKET #%04u\n", entry->pktCounter);
    Serial.println("────────────────────────────────────────");
    Serial.printf ("  Type   : 0x%02X\n",  entry->pktType);
    Serial.printf ("  RSSI   : %d dBm\n",  entry->rssi);
    Serial.printf ("  Length : %d bytes\n",(int)strlen((char*)entry->data));
    Serial.printf ("  Data   : %s\n",      (char*)entry->data);
    Serial.println("════════════════════════════════════════\n");

    entry->valid = false;  // mark slot free
  }
}


// ════════════════════════════════════════════════════════════
//  NODE REGISTRY
// ════════════════════════════════════════════════════════════
struct NodeInfo {
  uint32_t      id;
  String        name;
  String        data;
  uint32_t      parentId;
  unsigned long lastSeen;
};

NodeInfo nodes[50];
int nodeCount = 0;

NodeInfo* getNode(uint32_t id, String name = "", uint32_t parentId = 0) {
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].id == id) return &nodes[i];
  }
  nodes[nodeCount].id       = id;
  nodes[nodeCount].name     = name;
  nodes[nodeCount].parentId = parentId;
  nodes[nodeCount].lastSeen = millis();
  nodeCount++;
  return &nodes[nodeCount - 1];
}

void cleanupNodes() {
  for (int i = 0; i < nodeCount; i++) {
    if (millis() - nodes[i].lastSeen > 15000) {
      Serial.println("[MESH] Removing inactive node: " + String(nodes[i].id));
      for (int j = i; j < nodeCount - 1; j++) nodes[j] = nodes[j + 1];
      nodeCount--;
      i--;
    }
  }
}


// ════════════════════════════════════════════════════════════
//  MESH CALLBACKS
// ════════════════════════════════════════════════════════════
void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("[MESH] NODE CONNECTED: %u  Total: %d\n", nodeId, nodeCount);
}

void droppedConnectionCallback(uint32_t nodeId) {
  Serial.printf("[MESH] NODE DISCONNECTED: %u\n", nodeId);
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].id == nodeId) {
      for (int j = i; j < nodeCount - 1; j++) nodes[j] = nodes[j + 1];
      nodeCount--;
      break;
    }
  }
  Serial.printf("[MESH] Total Clients: %d\n", nodeCount);
}

// Message format:
//   "NodeName Parent:XXXX Temp:XX CO:0 CO2:0 Fire:0 Heart:0
//    Lat:X.XXXXXX Lon:XX.XXXXXX Heading:XXX Dist:XXXX Sats:X
//    GPS:X Image:0 Panic:X RSSI:-XX"
void receivedCallback(uint32_t from, String &msg) {
  int idx         = msg.indexOf(" ");
  String nodeName = msg.substring(0, idx);

  int pStart        = msg.indexOf("Parent:") + 7;
  int pEnd          = msg.indexOf(" ", pStart);
  uint32_t parentId = msg.substring(pStart, pEnd).toInt();
  String data       = msg.substring(pEnd + 1);

  NodeInfo* n = getNode(from, nodeName, parentId);
  n->name     = nodeName;
  n->parentId = parentId;
  n->data     = data;
  n->lastSeen = millis();

  Serial.println("\n==============================");
  Serial.println("[MESH] TEAM MEMBER DATA");
  Serial.println("------------------------------");
  Serial.printf("Name: %s  NodeID: %u\n", nodeName.c_str(), from);

  if (parentId == 0 || parentId == 2147483647)
    Serial.println("Connection: DIRECT TO BASE");
  else
    Serial.printf("Connection: VIA NODE %u\n", parentId);

  Serial.println("Data: " + data);

  if (data.indexOf("Panic:1") >= 0) {
    Serial.println("\n!!!!! SOS ALERT !!!!!");
    Serial.printf("TEAM MEMBER: %s  — PANIC BUTTON PRESSED\n", nodeName.c_str());
  }
  Serial.printf("TOTAL CLIENTS: %d\n", nodeCount);
}


// ════════════════════════════════════════════════════════════
//  WEB DASHBOARD
// ════════════════════════════════════════════════════════════
void handleRoot() {
  String page = "<html><head>";
  page += "<meta http-equiv='refresh' content='2'>";
  page += "<style>body{font-family:sans-serif;}table{border-collapse:collapse;width:100%;}";
  page += "td,th{padding:6px 10px;border:1px solid #aaa;font-size:13px;}";
  page += "th{background:#eee;}.sos{background:#ff4444;color:#fff;font-weight:bold;}</style>";
  page += "</head><body>";
  page += "<h2>ESP32 Mesh Team Dashboard</h2>";
  page += "<p>Transport: <b>";
#if USE_ESP_NOW_ONLY
  page += "ESP-NOW/Mesh only";
#else
  page += "ESP-NOW/Mesh + LoRa (dual)";
#endif
  page += "</b></p>";
  page += "Total Clients: " + String(nodeCount) + "<br><br>";
  page += "<table><tr><th>Name</th><th>Node ID</th><th>Connection</th><th>Status</th><th>Data</th></tr>";

  for (int i = 0; i < nodeCount; i++) {
    String status     = (millis() - nodes[i].lastSeen > 10000) ? "OFFLINE" : "ONLINE";
    String parentInfo = (nodes[i].parentId == 0 || nodes[i].parentId == 2147483647)
                        ? "Direct" : "Via " + String(nodes[i].parentId);
    bool isSOS = nodes[i].data.indexOf("Panic:1") >= 0;
    page += "<tr" + String(isSOS ? " class='sos'" : "") + ">";
    page += "<td>" + nodes[i].name + "</td>";
    page += "<td>" + String(nodes[i].id) + "</td>";
    page += "<td>" + parentInfo + "</td>";
    page += "<td>" + status + "</td>";
    page += "<td>" + nodes[i].data + "</td>";
    page += "</tr>";
  }
  page += "</table></body></html>";
  server.send(200, "text/html", page);
}


// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onDroppedConnection(&droppedConnectionCallback);

  // LoRa RX — only when both transports active
#if !USE_ESP_NOW_ONLY
  loraReady = initLoRaRX();
  if (!loraReady) Serial.println("[LoRa] DISABLED — init failed, LoRa RX skipped");
#else
  Serial.println("[LoRa] Disabled by USE_ESP_NOW_ONLY flag — mesh only");
#endif

  // Dashboard softAP (separate from mesh WiFi)
  WiFi.softAP("ESP32_DASH", "12345678");
  Serial.printf("[WiFi] Dashboard IP: %s\n", WiFi.softAPIP().toString().c_str());

  server.on("/", handleRoot);
  server.begin();

  Serial.printf("[READY] USE_ESP_NOW_ONLY=%s\n", USE_ESP_NOW_ONLY ? "true" : "false");
}


// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  mesh.update();
  cleanupNodes();
  server.handleClient();

#if !USE_ESP_NOW_ONLY
  if (loraReady) processLoRaPackets();
#endif
}
