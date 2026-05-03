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
 *    Connect to WiFi "BELT_MESH_9271" (pw: 12345678)
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
 *    SSID     : BELT_MESH_9271   Password : 12345678   Port : 5555
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
#include <ArduinoJson.h>

// ── Transport decision ────────────────────────────────────────
#define USE_ESP_NOW_ONLY false   // kept for legacy reference; LoRa RX is always active

// ── Mesh config ───────────────────────────────────────────────
#define MESH_PREFIX   "BELT_BASE_9271_X9"
#define MESH_PASSWORD "12345678"
#define MESH_PORT     5555

// ── LoRa pins ─────────────────────────────────────────────────
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
//  LORA RX RING BUFFER
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

void onLoRaReceive(int packetSize) {
  if (packetSize < 4) return;
  int nextHead = (loraRingHead + 1) % LORA_RING_SIZE;
  if (nextHead == loraRingTail) return;
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
  if (!LoRa.begin(LORA_FREQUENCY)) { Serial.println("[LoRa] Init failed!"); return false; }
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  // No ISR — use polling via LoRa.parsePacket() to avoid
  // SPI contention with painlessMesh FreeRTOS tasks.
  Serial.printf("[LoRa] Frequency: %.0f Hz — polling mode\n", (float)LORA_FREQUENCY);
  Serial.println("[LoRa] Init success!");
  return true;
}

static void updateLatestApiDataFromLoRa(const char* payload, int loraRssi); // forward decl

void processLoRaPackets() {
  int packetSize = LoRa.parsePacket();
  if (packetSize < 4) return;

  int      rssi       = LoRa.packetRssi();
  uint8_t  pktType    = LoRa.read();
  uint16_t pktCounter = ((uint16_t)LoRa.read() << 8) | LoRa.read();
  uint8_t  dataLen    = LoRa.read();
  if (dataLen > LORA_MAX_DATA - 1) dataLen = LORA_MAX_DATA - 1;

  char data[LORA_MAX_DATA];
  int idx = 0;
  while (LoRa.available() && idx < dataLen)
    data[idx++] = (char)LoRa.read();
  data[idx] = '\0';

  blinkLED(2, 80, 80);
  Serial.println("\n════════════════════════════════════════");
  Serial.printf ("  [LoRa RX] PKT #%04u  RSSI: %d dBm\n", pktCounter, rssi);
  Serial.printf ("  Data: %s\n", data);
  Serial.println("════════════════════════════════════════\n");
  updateLatestApiDataFromLoRa(data, rssi);
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
static void updateLatestApiData(const NodeInfo& n);

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

// ── Helper parsers ────────────────────────────────────────────
static String extractField(const String& data, const char* key) {
  int start = data.indexOf(key);
  if (start < 0) return "-";
  start += strlen(key);
  int end = data.indexOf(" ", start);
  if (end < 0) end = data.length();
  return data.substring(start, end);
}
static float fieldToFloat(const String& data, const char* key, float def = 0.0f) {
  String v = extractField(data, key);
  if (v == "-") return def;
  return v.toFloat();
}
static int fieldToInt(const String& data, const char* key, int def = 0) {
  String v = extractField(data, key);
  if (v == "-") return def;
  return v.toInt();
}
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
  updateLatestApiData(*n);

  Serial.println("\n==============================");
  Serial.println("[MESH] TEAM MEMBER DATA");
  Serial.println("------------------------------");
  Serial.printf("Name: %s  NodeID: %u\n", nodeName.c_str(), from);
  if (parentId == 0 || parentId == 2147483647)
    Serial.println("Connection: DIRECT TO BASE");
  else
    Serial.printf("Connection: VIA NODE %u\n", parentId);
  Serial.println("Data: " + data);
  {
    String blatStr = extractField(data, "BaseLat:");
    String blngStr = extractField(data, "BaseLng:");
    int    bokVal  = fieldToInt(data, "BaseOk:");
    int    navVal  = fieldToInt(data, "NavOn:");
    Serial.printf("Base: %s, %s  [%s]   Nav: %s\n",
      blatStr.c_str(), blngStr.c_str(),
      bokVal ? "LOCKED" : "CAPTURING",
      navVal ? "ACTIVE" : "STANDBY");
  }
  if (data.indexOf("Panic:1") >= 0) {
    Serial.println("\n!!!!! SOS ALERT !!!!!");
    Serial.printf("TEAM MEMBER: %s  — PANIC BUTTON PRESSED\n", nodeName.c_str());
  }
  Serial.printf("TOTAL CLIENTS: %d\n", nodeCount);
}

static String latestDataStr = "{}";
static int packetsReceived = 0;
static unsigned long lastPacketMs = 0;

static void updateLatestApiData(const NodeInfo& n) {
  StaticJsonDocument<1280> doc;
  const String& d = n.data;

  float lat = fieldToFloat(d, "Lat:");
  float lon = fieldToFloat(d, "Lon:");
  float alt = fieldToFloat(d, "Alt:");
  float spd = fieldToFloat(d, "Spd:");
  int sats  = fieldToInt(d, "Sats:");
  int gpsFix= fieldToInt(d, "GPS:");
  float hdg = fieldToFloat(d, "Hdg:");
  float rel = fieldToFloat(d, "Rel:");
  float dist= fieldToFloat(d, "Dist:");
  float temp= fieldToFloat(d, "Temp:");
  float hr  = fieldToFloat(d, "HR:");
  float ax  = fieldToFloat(d, "Ax:");
  float ay  = fieldToFloat(d, "Ay:");
  float az  = fieldToFloat(d, "Az:");
  float gx  = fieldToFloat(d, "Gx:");
  float gy  = fieldToFloat(d, "Gy:");
  float gz  = fieldToFloat(d, "Gz:");
  int panic = fieldToInt(d, "Panic:");
  int rssi  = fieldToInt(d, "RSSI:");
  String navState = extractField(d, "Nav:");
  float  baseLat   = fieldToFloat(d, "BaseLat:");
  float  baseLng   = fieldToFloat(d, "BaseLng:");
  int    baseOk    = fieldToInt(d, "BaseOk:");
  int    navOn     = fieldToInt(d, "NavOn:");

  JsonObject gps = doc.createNestedObject("gps");
  gps["valid"]      = gpsFix == 1;
  gps["lat"]        = lat;
  gps["lng"]        = lon;
  gps["alt"]        = alt;
  gps["speed"]      = spd;
  gps["satellites"] = sats;

  JsonObject imu = doc.createNestedObject("imu");
  imu["valid"]   = true;
  imu["accel_x"] = ax;
  imu["accel_y"] = ay;
  imu["accel_z"] = az;
  imu["gyro_x"]  = gx;
  imu["gyro_y"]  = gy;
  imu["gyro_z"]  = gz;
  imu["temp"]    = temp;

  JsonObject nav = doc.createNestedObject("nav");
  nav["fused_heading"] = hdg;
  nav["bearing"]       = hdg;
  nav["rel_bearing"]   = rel;
  nav["distance"]      = dist;
  nav["state"]         = navState;
  nav["heading_src"]   = "MESH";
  nav["bias_z"]        = gz;
  nav["active"]        = navOn == 1;

  JsonObject base = doc.createNestedObject("base");
  base["lat"]    = baseLat;
  base["lng"]    = baseLng;
  base["locked"] = baseOk == 1;

  JsonObject hrObj = doc.createNestedObject("hr");
  hrObj["valid"]  = true;
  hrObj["finger"] = hr > 0.0f;
  hrObj["bpm"]    = hr;

  JsonObject meta = doc.createNestedObject("meta");
  meta["node_name"] = n.name;
  meta["node_id"]   = n.id;
  meta["rssi"]      = rssi;
  meta["panic"]     = panic;

  packetsReceived++;
  doc["timestamp"] = millis();
  doc["packetNum"] = packetsReceived;

  latestDataStr = "";
  serializeJson(doc, latestDataStr);
  lastPacketMs = millis();
}


// ════════════════════════════════════════════════════════════
//  LORA FIELD EXTRACTOR  (key=value,... format)
// ════════════════════════════════════════════════════════════
static String extractLoRaField(const String& data, const char* key) {
  String searchKey = String(key) + "=";
  int start = data.indexOf(searchKey);
  if (start < 0) return "-";
  start += searchKey.length();
  int end = data.indexOf(",", start);
  if (end < 0) end = data.length();
  return data.substring(start, end);
}

// ════════════════════════════════════════════════════════════
//  UPDATE API FROM LORA PACKET
//  LoRa payload: name=X,lat=F,lon=F,hdg=F,dist=F,rel=F,
//                btemp=F,sats=I,gps=I,atemp=F,thmax=F,
//                co2=F,co=I,vstatus=S,vdist=F,alert=S,
//                buzzer=I,panic=I,rssi=I,
//                blat=F,blng=F,bok=I,nav=I
// ════════════════════════════════════════════════════════════
static void updateLatestApiDataFromLoRa(const char* payload, int loraRssi) {
  String d = String(payload);

  float  lat    = extractLoRaField(d, "lat").toFloat();
  float  lon    = extractLoRaField(d, "lon").toFloat();
  float  hdg    = extractLoRaField(d, "hdg").toFloat();
  float  dist   = extractLoRaField(d, "dist").toFloat();
  float  rel    = extractLoRaField(d, "rel").toFloat();
  float  btemp  = extractLoRaField(d, "btemp").toFloat();
  int    sats   = extractLoRaField(d, "sats").toInt();
  int    gpsFix = extractLoRaField(d, "gps").toInt();
  int    panic  = extractLoRaField(d, "panic").toInt();
  int    nodeRssi = extractLoRaField(d, "rssi").toInt();
  float  blat   = extractLoRaField(d, "blat").toFloat();
  float  blng   = extractLoRaField(d, "blng").toFloat();
  int    bok    = extractLoRaField(d, "bok").toInt();
  int    navOn  = extractLoRaField(d, "nav").toInt();
  String navSt  = extractLoRaField(d, "navst");
  if (navSt == "-") navSt = navOn ? "FORWARD" : "STANDBY";
  String name   = extractLoRaField(d, "name");
  if (name == "-") name = "LORA_NODE";

  StaticJsonDocument<1280> doc;

  JsonObject gps = doc.createNestedObject("gps");
  gps["valid"]      = gpsFix == 1;
  gps["lat"]        = lat;
  gps["lng"]        = lon;
  gps["alt"]        = 0.0f;
  gps["speed"]      = 0.0f;
  gps["satellites"] = sats;

  JsonObject imu = doc.createNestedObject("imu");
  imu["valid"]   = false;
  imu["accel_x"] = 0.0f; imu["accel_y"] = 0.0f; imu["accel_z"] = 0.0f;
  imu["gyro_x"]  = 0.0f; imu["gyro_y"]  = 0.0f; imu["gyro_z"]  = 0.0f;
  imu["temp"]    = btemp;

  JsonObject nav = doc.createNestedObject("nav");
  nav["fused_heading"] = hdg;
  nav["bearing"]       = hdg;
  nav["rel_bearing"]   = rel;
  nav["distance"]      = dist;
  nav["state"]         = navSt;
  nav["heading_src"]   = "LORA";
  nav["bias_z"]        = 0.0f;
  nav["active"]        = navOn == 1;

  JsonObject base = doc.createNestedObject("base");
  base["lat"]    = blat;
  base["lng"]    = blng;
  base["locked"] = bok == 1;

  JsonObject hrObj = doc.createNestedObject("hr");
  hrObj["valid"]  = false;
  hrObj["finger"] = false;
  hrObj["bpm"]    = 0;

  JsonObject meta = doc.createNestedObject("meta");
  meta["node_name"] = name;
  meta["node_id"]   = 0;
  meta["rssi"]      = loraRssi;   // LoRa RSSI at base station RX
  meta["panic"]     = panic;

  packetsReceived++;
  doc["timestamp"]  = millis();
  doc["packetNum"]  = packetsReceived;
  doc["transport"]  = "LORA";

  latestDataStr = "";
  serializeJson(doc, latestDataStr);
  lastPacketMs = millis();
}


// ════════════════════════════════════════════════════════════
//  TACTICAL HUD DASHBOARD
// ════════════════════════════════════════════════════════════
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MESH OPS — TACTICAL HUD</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:     #03060d;
  --bg2:    #050a15;
  --panel:  #060c1a;
  --panel2: #040810;
  --border: #0c1d38;
  --border2:#142a4a;
  --green:  #00e676;
  --green2: #00ff9d;
  --amber:  #ffab00;
  --red:    #ff3d57;
  --blue:   #29b6f6;
  --purple: #b388ff;
  --cyan:   #00e5ff;
  --dim:    #1a2e50;
  --text:   #b8cce8;
  --muted:  #3a5070;
  --font:   'Courier New','Lucida Console',monospace;
}

/* Scanline overlay */
body::before{
  content:'';position:fixed;inset:0;pointer-events:none;z-index:9999;
  background:repeating-linear-gradient(
    0deg,transparent,transparent 3px,
    rgba(0,230,118,.012) 3px,rgba(0,230,118,.012) 4px
  );
}
/* CRT vignette */
body::after{
  content:'';position:fixed;inset:0;pointer-events:none;z-index:9998;
  background:radial-gradient(ellipse at center,transparent 60%,rgba(0,0,0,.6) 100%);
}

html,body{
  font-family:var(--font);background:var(--bg);
  color:var(--text);min-height:100vh;overflow-x:hidden;
}

/* ── PANIC BANNER ── */
#panicBanner{
  display:none;position:fixed;top:0;left:0;right:0;z-index:10000;
  background:var(--red);color:#fff;text-align:center;
  padding:12px;font-size:1rem;letter-spacing:4px;font-weight:bold;
  text-shadow:0 0 20px rgba(255,255,255,.8);
  animation:panicFlash .5s infinite;
}
@keyframes panicFlash{0%,100%{opacity:1}50%{opacity:.6}}

/* ── TOPBAR ── */
.topbar{
  position:sticky;top:0;z-index:100;
  display:flex;align-items:center;justify-content:space-between;
  padding:10px 18px;
  background:rgba(3,6,13,.97);
  border-bottom:1px solid var(--border2);
  box-shadow:0 0 40px rgba(0,230,118,.05),0 1px 0 rgba(0,230,118,.08);
}
.brand{display:flex;flex-direction:column;gap:3px}
.brand-title{
  font-size:1.05rem;letter-spacing:5px;color:var(--green);
  text-transform:uppercase;text-shadow:0 0 20px rgba(0,230,118,.5);
  display:flex;align-items:center;gap:10px;
}
.brand-title::before{
  content:'▶';color:var(--green);font-size:.7rem;
  animation:blink 2s infinite;
}
.brand-sub{font-size:.65rem;letter-spacing:3px;color:var(--muted);padding-left:18px}
.topbar-right{display:flex;gap:10px;align-items:center;flex-wrap:wrap}

.pill{
  display:flex;align-items:center;gap:7px;
  padding:5px 11px;border:1px solid var(--border2);
  border-radius:3px;background:rgba(6,12,26,.9);
  font-size:.68rem;letter-spacing:1.5px;white-space:nowrap;
}
.pill-label{color:var(--muted)}
.pill-val{color:var(--text)}
.dot{width:7px;height:7px;border-radius:50%;background:var(--red)}
.dot.live{background:var(--green);box-shadow:0 0 10px rgba(0,230,118,.9);animation:blink 1.4s infinite}
.dot.warn{background:var(--amber);box-shadow:0 0 8px rgba(255,171,0,.7);animation:blink .8s infinite}

@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}

/* ── GRID ── */
.wrap{max-width:1500px;margin:0 auto;padding:12px;display:flex;flex-direction:column;gap:12px}
.row{display:grid;gap:12px}
.r1{grid-template-columns:1fr 260px 1fr}
.r2{grid-template-columns:1fr 1fr 1fr 1fr}
.r3{grid-template-columns:2fr 1fr 1fr}

/* ── PANEL BASE ── */
.panel{
  background:linear-gradient(145deg,var(--panel) 0%,var(--panel2) 100%);
  border:1px solid var(--border);
  border-radius:5px;
  padding:14px;
  position:relative;
  overflow:hidden;
}
/* corner brackets */
.panel::before,.panel::after{
  content:'';position:absolute;width:12px;height:12px;
  border-color:rgba(0,230,118,.3);border-style:solid;
}
.panel::before{top:0;left:0;border-width:1px 0 0 1px}
.panel::after{bottom:0;right:0;border-width:0 1px 1px 0}

/* inner glow on hover */
.panel:hover{border-color:var(--border2);box-shadow:inset 0 0 30px rgba(0,230,118,.03)}

.ph{
  display:flex;align-items:flex-start;justify-content:space-between;
  margin-bottom:12px;padding-bottom:9px;
  border-bottom:1px solid var(--border);
}
.ph-left{display:flex;flex-direction:column;gap:2px}
.ph-label{font-size:.6rem;letter-spacing:2.5px;color:var(--muted);text-transform:uppercase}
.ph-title{font-size:.82rem;letter-spacing:2px;color:var(--text);text-transform:uppercase}

.tag{
  font-size:.62rem;letter-spacing:1px;padding:3px 8px;
  border:1px solid var(--border2);border-radius:2px;
  color:var(--muted);background:rgba(0,0,0,.5);
  white-space:nowrap;
}
.tag.g{border-color:rgba(0,230,118,.45);color:var(--green);background:rgba(0,230,118,.07)}
.tag.a{border-color:rgba(255,171,0,.45);color:var(--amber);background:rgba(255,171,0,.07)}
.tag.r{border-color:rgba(255,61,87,.45);color:var(--red);background:rgba(255,61,87,.07)}
.tag.b{border-color:rgba(41,182,246,.45);color:var(--blue);background:rgba(41,182,246,.07)}
.tag.p{border-color:rgba(179,136,255,.45);color:var(--purple);background:rgba(179,136,255,.07)}

/* ── DATA ROW ── */
.drow{
  display:flex;justify-content:space-between;align-items:center;
  font-size:.73rem;padding:5px 0;
  border-bottom:1px solid rgba(12,29,56,.6);
}
.drow:last-child{border:none}
.dk{color:var(--muted);letter-spacing:1px;font-size:.67rem}
.dv{color:var(--text)}
.dv.g{color:var(--green)}
.dv.a{color:var(--amber)}
.dv.r{color:var(--red)}
.dv.b{color:var(--blue)}
.dv.p{color:var(--purple)}
.dv.c{color:var(--cyan)}

/* ── BIG NUMBER ── */
.bignum{
  font-size:2.8rem;letter-spacing:1px;text-align:center;
  line-height:1;transition:color .3s;
}
.bignum.g{color:var(--green);text-shadow:0 0 30px rgba(0,230,118,.35)}
.bignum.a{color:var(--amber);text-shadow:0 0 30px rgba(255,171,0,.35)}
.bignum.r{color:var(--red);text-shadow:0 0 30px rgba(255,61,87,.35)}
.bignum.b{color:var(--blue);text-shadow:0 0 25px rgba(41,182,246,.35)}
.bignum.m{color:var(--muted)}
.unit{font-size:.65rem;letter-spacing:2px;color:var(--muted);text-align:center;margin-top:3px}

/* ── BAR ── */
.bar-track{
  height:5px;background:rgba(255,255,255,.04);
  border:1px solid var(--border);border-radius:2px;overflow:hidden;
}
.bar-fill{
  height:100%;border-radius:2px;transition:width .7s ease;
}
.bar-green{background:linear-gradient(90deg,var(--green),var(--green2));box-shadow:0 0 8px rgba(0,230,118,.4)}
.bar-amber{background:linear-gradient(90deg,var(--amber),#ffd54f);box-shadow:0 0 8px rgba(255,171,0,.4)}
.bar-red{background:linear-gradient(90deg,var(--red),#ff6d7e);box-shadow:0 0 8px rgba(255,61,87,.4)}

/* ── NAV STATE ── */
.nav-center{display:flex;flex-direction:column;align-items:center;gap:10px}
.nav-state{
  font-size:1.4rem;letter-spacing:3px;text-transform:uppercase;
  text-align:center;transition:all .35s;padding:4px 0;
}
.ns-fwd  {color:var(--green2);text-shadow:0 0 25px rgba(0,255,157,.5)}
.ns-turn {color:var(--amber); text-shadow:0 0 20px rgba(255,171,0,.5)}
.ns-back {color:var(--red);   text-shadow:0 0 20px rgba(255,61,87,.5)}
.ns-arr  {color:var(--green); text-shadow:0 0 35px rgba(0,230,118,.8)}
.ns-none {color:var(--muted)}
.nav-sub{font-size:.68rem;letter-spacing:2.5px;color:var(--muted);text-align:center}

/* ── ARROW SVG ── */
#dirArrowSvg{transition:transform .45s cubic-bezier(.4,0,.2,1)}
.arrow-wrap{display:flex;flex-direction:column;align-items:center;gap:6px}
.arrow-legend{font-size:.63rem;letter-spacing:1.5px;color:var(--muted)}

/* ── COMPASS ── */
.compass-vals{
  display:flex;gap:0;justify-content:center;
  margin-top:8px;border:1px solid var(--border);border-radius:3px;
  overflow:hidden;
}
.cv-box{
  flex:1;text-align:center;padding:6px 4px;
  border-right:1px solid var(--border);
}
.cv-box:last-child{border:none}
.cv-label{font-size:.58rem;letter-spacing:1px;color:var(--muted)}
.cv-val{font-size:.85rem;margin-top:3px}

/* ── GPS GRID ── */
.gps-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px}
.gps-cell{
  background:rgba(0,0,0,.4);border:1px solid var(--border);
  border-radius:3px;padding:8px;
}
.gc-label{font-size:.6rem;letter-spacing:1.5px;color:var(--muted)}
.gc-val{font-size:.88rem;margin-top:4px;color:var(--text)}
.sat-bars{display:flex;align-items:flex-end;gap:2px;height:20px}
.sat-bar{width:9px;background:var(--dim);border-radius:1px;transition:all .5s}
.sat-bar.lit{background:var(--green);box-shadow:0 0 5px rgba(0,230,118,.5)}

/* ── IMU ── */
.tilt-wrap{display:flex;flex-direction:column;align-items:center;gap:8px}
.axis-row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:5px;width:100%}
.ax-box{
  background:rgba(0,0,0,.4);border:1px solid var(--border);
  border-radius:3px;text-align:center;padding:6px 3px;
}
.ax-lbl{font-size:.58rem;letter-spacing:1.5px;color:var(--muted)}
.ax-val{font-size:.82rem;margin-top:3px}

/* ── GYRO ── */
.gyro-wrap{display:flex;flex-direction:column;gap:8px;margin-top:2px}
.gbar{display:flex;align-items:center;gap:8px}
.gbar-lbl{font-size:.65rem;letter-spacing:1px;color:var(--muted);width:14px;text-align:center}
.gbar-track{
  flex:1;height:7px;background:rgba(255,255,255,.04);
  border:1px solid var(--border);border-radius:3px;
  position:relative;overflow:visible;
}
.gbar-fill{position:absolute;top:0;height:100%;border-radius:3px;transition:all .15s}
.gbar-center{position:absolute;top:-2px;left:50%;width:1px;height:11px;background:var(--dim)}
.gbar-val{font-size:.68rem;width:48px;text-align:right}

/* ── HEART ── */
.hr-wrap{display:flex;flex-direction:column;align-items:center;gap:6px}
.hr-bpm{font-size:3.2rem;letter-spacing:2px;text-align:center;transition:all .3s}
.hr-bpm.on{color:var(--red);text-shadow:0 0 30px rgba(255,61,87,.5)}
.hr-bpm.off{color:var(--muted)}
.hr-status{font-size:.68rem;letter-spacing:1.5px;text-align:center}
.hr-status.on{color:var(--red)}
.hr-status.off{color:var(--muted)}

/* ── NODE INFO ── */
.node-grid{display:flex;flex-direction:column;gap:5px}
.rssi-bar-wrap{margin-top:4px}
.rssi-segments{display:flex;gap:3px;align-items:flex-end;height:24px;margin-top:4px}
.rseg{
  flex:1;border-radius:2px;transition:all .4s;
  background:var(--dim);
}
.rseg.lit-g{background:var(--green);box-shadow:0 0 5px rgba(0,230,118,.4)}
.rseg.lit-a{background:var(--amber);box-shadow:0 0 5px rgba(255,171,0,.4)}
.rseg.lit-r{background:var(--red);  box-shadow:0 0 5px rgba(255,61,87,.4)}

/* ── TICKER BOTTOM ── */
.ticker{
  display:flex;gap:0;
  border:1px solid var(--border);border-radius:3px;overflow:hidden;
  flex-wrap:wrap;
}
.tick-cell{
  flex:1;min-width:80px;padding:7px 10px;
  border-right:1px solid var(--border);
  background:rgba(0,0,0,.3);
}
.tick-cell:last-child{border:none}
.tick-k{font-size:.58rem;letter-spacing:1.5px;color:var(--muted);text-transform:uppercase}
.tick-v{font-size:.8rem;margin-top:3px;color:var(--text)}

/* ── PKT counter ── */
.pkt-counter{
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:2px;
}
.pkt-num{
  font-size:3rem;color:var(--cyan);
  text-shadow:0 0 20px rgba(0,229,255,.4);
  line-height:1;
}
.pkt-sub{font-size:.6rem;letter-spacing:2px;color:var(--muted)}

/* ── Raw JSON ── */
.raw-box{
  background:rgba(0,0,0,.6);border:1px solid var(--border);
  border-radius:3px;padding:10px;
  font-size:.68rem;color:#3a6a9a;
  max-height:180px;overflow:auto;
  white-space:pre-wrap;word-break:break-all;line-height:1.55;
}
.raw-box::-webkit-scrollbar{width:4px}
.raw-box::-webkit-scrollbar-thumb{background:var(--border2);border-radius:2px}

/* ── Heading src pill ── */
.hsrc{
  display:inline-flex;align-items:center;gap:4px;
  font-size:.65rem;letter-spacing:1.5px;padding:3px 8px;
  border:1px solid;border-radius:2px;
}
.hsrc.MESH{border-color:rgba(179,136,255,.5);color:var(--purple)}
.hsrc.GPS {border-color:rgba(41,182,246,.5); color:var(--blue)}
.hsrc.GYRO{border-color:rgba(0,229,255,.5);  color:var(--cyan)}
.hsrc.NONE{border-color:var(--border);       color:var(--muted)}

@media(max-width:1100px){
  .r1,.r2{grid-template-columns:1fr 1fr}
  .r3{grid-template-columns:1fr 1fr}
}
@media(max-width:640px){
  .r1,.r2,.r3{grid-template-columns:1fr}
  .topbar{flex-direction:column;gap:8px;align-items:flex-start}
}
</style>
</head>
<body>

<!-- PANIC BANNER -->
<div id="panicBanner">⚠ SOS — PANIC BUTTON ACTIVATED — <span id="panicNode">NODE</span> NEEDS ASSISTANCE ⚠</div>

<!-- TOPBAR -->
<header class="topbar">
  <div class="brand">
    <div class="brand-title">MESH OPS — TACTICAL HUD</div>
    <div class="brand-sub">PAINLESSMESH RECEIVER &nbsp;|&nbsp; REAL-TIME FIELD TELEMETRY &nbsp;|&nbsp; 192.168.4.1</div>
  </div>
  <div class="topbar-right">
    <div class="pill"><div class="dot" id="connDot"></div><span id="connText" class="pill-val">WAITING...</span></div>
    <div class="pill"><span class="pill-label">NODE</span><span class="pill-val" id="topNode">--</span></div>
    <div class="pill"><span class="pill-label">PKT</span><span class="pill-val" id="topPkt" style="color:var(--cyan)">0</span></div>
    <div class="pill"><span class="pill-label">AGE</span><span class="pill-val" id="topAge" style="color:var(--amber)">--</span></div>
  </div>
</header>

<main class="wrap">

  <!-- ROW 1: Nav Cue | Compass | Distance -->
  <div class="row r1">

    <!-- Navigation direction -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Navigation</div>
          <div class="ph-title">Direction Cue</div>
        </div>
        <div id="hdgSrcBadge" class="hsrc NONE">HDG: --</div>
      </div>
      <div class="nav-center">
        <div class="arrow-wrap">
          <svg id="dirArrowSvg" width="88" height="88" viewBox="-44 -44 88 88">
            <defs>
              <filter id="arrowGlow">
                <feGaussianBlur stdDeviation="3.5" result="b"/>
                <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
              </filter>
            </defs>
            <circle r="40" fill="rgba(0,0,0,.4)" stroke="#0c1d38" stroke-width="1"/>
            <polygon id="dirArrow"
              points="0,-34 11,-11 4,-11 4,34 -4,34 -4,-11 -11,-11"
              fill="#3a5070" filter="url(#arrowGlow)"/>
            <circle r="4" fill="none" stroke="#1a3050" stroke-width="1.5"/>
          </svg>
          <div class="arrow-legend" id="relBearLabel">REL: --</div>
        </div>
        <div class="nav-state ns-none" id="navState">AWAITING GPS</div>
        <div class="nav-sub" id="navSub">--</div>
      </div>
    </div>

    <!-- Compass -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Orientation</div>
          <div class="ph-title">Compass Rose</div>
        </div>
        <span class="tag b">FUSED</span>
      </div>
      <div style="display:flex;flex-direction:column;align-items:center">
        <canvas id="compassCv" width="192" height="192"></canvas>
        <div class="compass-vals" style="width:100%">
          <div class="cv-box">
            <div class="cv-label">HEADING</div>
            <div class="cv-val g" id="cvHdg">---°</div>
          </div>
          <div class="cv-box">
            <div class="cv-label">TO BASE</div>
            <div class="cv-val b" id="cvBear">---°</div>
          </div>
          <div class="cv-box">
            <div class="cv-label">REL</div>
            <div class="cv-val a" id="cvRel">---°</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Distance -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Target Range</div>
          <div class="ph-title">Distance</div>
        </div>
        <span class="tag a" id="arrivedTag">EN ROUTE</span>
      </div>
      <div class="bignum g" id="distNum" style="margin:6px 0 2px">---</div>
      <div class="unit" id="distUnit">METRES TO BASE</div>
      <div class="bar-track" style="margin:10px 0 12px">
        <div class="bar-fill bar-green" id="distBar" style="width:0%"></div>
      </div>
      <div class="drow"><span class="dk">STATE</span><span class="dv" id="drState">--</span></div>
      <div class="drow"><span class="dk">BEARING</span><span class="dv b" id="drBearing">--</span></div>
      <div class="drow"><span class="dk">SPEED</span><span class="dv" id="drSpeed">--</span></div>
      <div class="drow"><span class="dk">ALTITUDE</span><span class="dv" id="drAlt">--</span></div>
      <div class="drow"><span class="dk">PACKETS</span><span class="dv c" id="drPkts">0</span></div>
      <div class="drow"><span class="dk">LAST RX</span><span class="dv" id="drAge">--</span></div>
    </div>
  </div>

  <!-- ROW 2: GPS | Tilt | Gyro | Heart -->
  <div class="row r2">

    <!-- GPS -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Satellite</div>
          <div class="ph-title">GPS Fix</div>
        </div>
        <span class="tag r" id="gpsTag">NO FIX</span>
      </div>
      <div class="gps-grid" style="margin-bottom:8px">
        <div class="gps-cell"><div class="gc-label">LATITUDE</div><div class="gc-val" id="gLat">--</div></div>
        <div class="gps-cell"><div class="gc-label">LONGITUDE</div><div class="gc-val" id="gLng">--</div></div>
        <div class="gps-cell"><div class="gc-label">ALTITUDE</div><div class="gc-val" id="gAlt">--</div></div>
        <div class="gps-cell"><div class="gc-label">SPEED</div><div class="gc-val" id="gSpd">--</div></div>
      </div>
      <div class="gps-cell" style="display:flex;justify-content:space-between;align-items:center">
        <div>
          <div class="gc-label">SATELLITES</div>
          <div class="gc-val" id="gSats">--</div>
        </div>
        <div class="sat-bars" id="satBars"></div>
      </div>
    </div>

    <!-- Base Station -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Return Point</div>
          <div class="ph-title">Base Station</div>
        </div>
        <span class="tag" id="baseLockTag">CAPTURING</span>
      </div>
      <div class="gps-grid" style="margin-bottom:8px">
        <div class="gps-cell"><div class="gc-label">BASE LAT</div><div class="gc-val b" id="baseLat">--</div></div>
        <div class="gps-cell"><div class="gc-label">BASE LNG</div><div class="gc-val b" id="baseLng">--</div></div>
      </div>
      <div style="display:flex;gap:6px;margin-bottom:8px">
        <div class="gps-cell" style="flex:1;text-align:center">
          <div class="gc-label">STATUS</div>
          <div class="gc-val" id="baseStatus">--</div>
        </div>
        <div class="gps-cell" style="flex:1;text-align:center">
          <div class="gc-label">NAVIGATION</div>
          <div class="gc-val" id="navActiveEl">--</div>
        </div>
      </div>
      <div style="border-top:1px solid var(--border);padding-top:8px">
        <div class="gc-label" style="margin-bottom:4px">DISTANCE TO BASE</div>
        <div style="display:flex;align-items:baseline;gap:6px">
          <div style="font-size:1.6rem;font-weight:700;color:var(--green);font-family:var(--mono)" id="bsDist">--</div>
          <div style="font-size:.7rem;color:var(--muted)">METRES</div>
        </div>
        <div style="margin-top:6px;display:flex;gap:6px">
          <div class="gps-cell" style="flex:1;text-align:center">
            <div class="gc-label">BEARING</div>
            <div class="gc-val p" id="bsBearing">--</div>
          </div>
          <div class="gps-cell" style="flex:1;text-align:center">
            <div class="gc-label">REL BEARING</div>
            <div class="gc-val a" id="bsRel">--</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Tilt / Accel -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Accelerometer</div>
          <div class="ph-title">Tilt / Accel</div>
        </div>
        <span class="tag" id="imuTag">IMU</span>
      </div>
      <div class="tilt-wrap">
        <canvas id="tiltCv" width="136" height="136"></canvas>
        <div class="axis-row">
          <div class="ax-box"><div class="ax-lbl">AX</div><div class="ax-val b" id="aX">--</div></div>
          <div class="ax-box"><div class="ax-lbl">AY</div><div class="ax-val p" id="aY">--</div></div>
          <div class="ax-box"><div class="ax-lbl">AZ</div><div class="ax-val g" id="aZ">--</div></div>
        </div>
      </div>
    </div>

    <!-- Gyro -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Gyroscope</div>
          <div class="ph-title">Angular Rate</div>
        </div>
        <span class="tag">rad/s</span>
      </div>
      <div style="font-size:.6rem;letter-spacing:1px;color:var(--muted);margin-bottom:10px">
        ROTATION RATE — DEADZONE ±0.01
      </div>
      <div class="gyro-wrap">
        <div class="gbar">
          <span class="gbar-lbl b">X</span>
          <div class="gbar-track">
            <div class="gbar-center"></div>
            <div class="gbar-fill" id="gbX" style="background:var(--blue)"></div>
          </div>
          <span class="gbar-val b" id="gvX">--</span>
        </div>
        <div class="gbar">
          <span class="gbar-lbl p">Y</span>
          <div class="gbar-track">
            <div class="gbar-center"></div>
            <div class="gbar-fill" id="gbY" style="background:var(--purple)"></div>
          </div>
          <span class="gbar-val p" id="gvY">--</span>
        </div>
        <div class="gbar">
          <span class="gbar-lbl g">Z</span>
          <div class="gbar-track">
            <div class="gbar-center"></div>
            <div class="gbar-fill" id="gbZ" style="background:var(--green)"></div>
          </div>
          <span class="gbar-val g" id="gvZ">--</span>
        </div>
      </div>
      <div style="margin-top:12px;border-top:1px solid var(--border);padding-top:10px">
        <div class="drow"><span class="dk">TEMP</span><span class="dv a" id="imuTemp">--</span></div>
        <div class="drow"><span class="dk">BIAS Z</span><span class="dv" id="imuBias">--</span></div>
      </div>
    </div>

    <!-- Heart Rate -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">MAX30102</div>
          <div class="ph-title">Heart Rate</div>
        </div>
        <span class="tag" id="hrTag">STANDBY</span>
      </div>
      <div class="hr-wrap">
        <div class="hr-bpm off" id="hrNum">---</div>
        <div class="unit">BPM</div>
        <div class="hr-status off" id="hrStatus">■ NO FINGER</div>
        <canvas id="hrCv" width="195" height="58" style="border:1px solid var(--border);border-radius:3px;margin-top:6px"></canvas>
      </div>
    </div>
  </div>

  <!-- ROW 3: Node Info + Ticker | Pkt Count | Raw JSON -->
  <div class="row r3">

    <!-- Node info + live ticker -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Mesh Node</div>
          <div class="ph-title">Live Feed</div>
        </div>
        <div class="pkt-counter">
          <div class="pkt-num" id="pktBig">0</div>
          <div class="pkt-sub">PACKETS RX</div>
        </div>
      </div>
      <!-- node meta -->
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:10px">
        <div class="gps-cell">
          <div class="gc-label">NODE NAME</div>
          <div class="gc-val p" id="metaName">--</div>
        </div>
        <div class="gps-cell">
          <div class="gc-label">NODE ID</div>
          <div class="gc-val" id="metaId">--</div>
        </div>
        <div class="gps-cell">
          <div class="gc-label">MESH RSSI</div>
          <div class="gc-val b" id="metaRssi">--</div>
        </div>
        <div class="gps-cell">
          <div class="gc-label">PANIC</div>
          <div class="gc-val" id="metaPanic">OK</div>
        </div>
      </div>
      <!-- RSSI bar -->
      <div>
        <div style="font-size:.6rem;letter-spacing:1.5px;color:var(--muted);margin-bottom:5px">RSSI SIGNAL STRENGTH</div>
        <div class="rssi-segments" id="rssiSegs">
          <!-- 10 segments filled by JS -->
        </div>
      </div>
      <!-- ticker row -->
      <div class="ticker" style="margin-top:12px">
        <div class="tick-cell"><div class="tick-k">TIME</div><div class="tick-v" id="tkTime">--</div></div>
        <div class="tick-cell"><div class="tick-k">GPS</div><div class="tick-v" id="tkGps">--</div></div>
        <div class="tick-cell"><div class="tick-k">HDG</div><div class="tick-v" id="tkHdg">--</div></div>
        <div class="tick-cell"><div class="tick-k">DIST</div><div class="tick-v" id="tkDist">--</div></div>
        <div class="tick-cell"><div class="tick-k">HR</div><div class="tick-v" id="tkHr">--</div></div>
        <div class="tick-cell"><div class="tick-k">UPTIME</div><div class="tick-v" id="tkUp">--</div></div>
      </div>
    </div>

    <!-- Packet stats -->
    <div class="panel" style="display:flex;flex-direction:column;justify-content:space-between">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Statistics</div>
          <div class="ph-title">Link Quality</div>
        </div>
      </div>
      <div style="flex:1;display:flex;flex-direction:column;justify-content:space-around">
        <div style="text-align:center">
          <div style="font-size:.6rem;letter-spacing:2px;color:var(--muted);margin-bottom:4px">PACKETS RECEIVED</div>
          <div class="bignum c" id="pktStat">0</div>
        </div>
        <div style="border-top:1px solid var(--border);padding-top:10px">
          <div class="drow"><span class="dk">UPTIME</span><span class="dv g" id="statUp">--</span></div>
          <div class="drow"><span class="dk">INTERVAL</span><span class="dv" id="statInterval">--</span></div>
          <div class="drow"><span class="dk">LAST RSSI</span><span class="dv b" id="statRssi">--</span></div>
          <div class="drow"><span class="dk">TRANSPORT</span><span class="dv p" id="statTransport">MESH</span></div>
        </div>
      </div>
    </div>

    <!-- Raw JSON -->
    <div class="panel">
      <div class="ph">
        <div class="ph-left">
          <div class="ph-label">Debug</div>
          <div class="ph-title">JSON Stream</div>
        </div>
        <span class="tag">/api/data</span>
      </div>
      <div class="raw-box" id="rawJson">{}</div>
    </div>
  </div>

</main>

<script>
// ── globals ─────────────────────────────────────────
var lastPktAt  = 0;
var startedAt  = Date.now();
var hrHistory  = new Array(100).fill(0);
var lastPktMs  = 0;
var prevPktMs  = 0;

// ── canvas contexts ──────────────────────────────────
var compassCtx = document.getElementById('compassCv').getContext('2d');
var tiltCtx    = document.getElementById('tiltCv').getContext('2d');
var hrCtx      = document.getElementById('hrCv').getContext('2d');

// ── compass ──────────────────────────────────────────
function drawCompass(hdg, bear) {
  var cx=96,cy=96,r=88;
  var c=compassCtx;
  c.clearRect(0,0,192,192);

  // bg
  var bg=c.createRadialGradient(cx,cy,0,cx,cy,r);
  bg.addColorStop(0,'#070f1e');
  bg.addColorStop(1,'#030609');
  c.beginPath();c.arc(cx,cy,r,0,Math.PI*2);
  c.fillStyle=bg;c.fill();
  c.strokeStyle='#0c1d38';c.lineWidth=1.5;c.stroke();

  // tick marks
  for(var i=0;i<360;i+=5){
    var a=(i-90)*Math.PI/180;
    var maj=i%90===0,mid=i%30===0,sml=i%10===0;
    var len=maj?16:mid?10:sml?6:3;
    c.beginPath();
    c.moveTo(cx+Math.cos(a)*(r-len),cy+Math.sin(a)*(r-len));
    c.lineTo(cx+Math.cos(a)*(r-1.5),cy+Math.sin(a)*(r-1.5));
    c.strokeStyle=maj?'#00e676':mid?'#1a3a3a':sml?'#0d2020':'#08161a';
    c.lineWidth=maj?1.5:1;
    c.stroke();
  }

  // cardinals
  var card=[{l:'N',a:0,col:'#00e676'},{l:'E',a:90,col:'#1a4a4a'},
            {l:'S',a:180,col:'#1a4a4a'},{l:'W',a:270,col:'#1a4a4a'}];
  c.font='bold 10px Courier New';
  c.textAlign='center';c.textBaseline='middle';
  card.forEach(function(cd){
    var a=(cd.a-90)*Math.PI/180;
    c.fillStyle=cd.col;
    c.fillText(cd.l,cx+Math.cos(a)*(r-23),cy+Math.sin(a)*(r-23));
  });

  // bearing line
  if(bear!==null){
    var ba=(bear-90)*Math.PI/180;
    c.setLineDash([5,4]);
    c.beginPath();c.moveTo(cx,cy);
    c.lineTo(cx+Math.cos(ba)*(r-5),cy+Math.sin(ba)*(r-5));
    c.strokeStyle='rgba(0,230,118,.45)';c.lineWidth=1.5;c.stroke();
    c.setLineDash([]);
    c.beginPath();
    c.arc(cx+Math.cos(ba)*(r-9),cy+Math.sin(ba)*(r-9),4,0,Math.PI*2);
    c.fillStyle='#00e676';c.fill();
  }

  // heading needle
  if(hdg!==null){
    var ha=(hdg-90)*Math.PI/180;
    // shaft
    c.beginPath();
    c.moveTo(cx+Math.cos(ha)*(r-7),cy+Math.sin(ha)*(r-7));
    c.lineTo(cx,cy);
    c.strokeStyle='#ffab00';c.lineWidth=2;c.stroke();
    // tip
    c.beginPath();
    c.arc(cx+Math.cos(ha)*(r-9),cy+Math.sin(ha)*(r-9),5,0,Math.PI*2);
    c.fillStyle='#ffab00';c.fill();
    // tail
    c.beginPath();
    var ha2=ha+Math.PI;
    c.moveTo(cx,cy);
    c.lineTo(cx+Math.cos(ha2)*20,cy+Math.sin(ha2)*20);
    c.strokeStyle='rgba(255,171,0,.25)';c.lineWidth=2;c.stroke();
  }

  // center
  c.beginPath();c.arc(cx,cy,5,0,Math.PI*2);c.fillStyle='#03060d';c.fill();
  c.beginPath();c.arc(cx,cy,3,0,Math.PI*2);c.fillStyle='#ffab00';c.fill();
}

// ── tilt bubble ──────────────────────────────────────
function drawTilt(ax,ay){
  var cx=68,cy=68,r=58;
  var c=tiltCtx;
  c.clearRect(0,0,136,136);

  c.beginPath();c.arc(cx,cy,r,0,Math.PI*2);
  c.fillStyle='#030609';c.fill();
  c.strokeStyle='#0c1d38';c.lineWidth=1.5;c.stroke();

  [.33,.66,1].forEach(function(f){
    c.beginPath();c.arc(cx,cy,r*f,0,Math.PI*2);
    c.strokeStyle='#0c1d38';c.lineWidth=1;c.stroke();
  });
  c.beginPath();
  c.moveTo(cx-r,cy);c.lineTo(cx+r,cy);
  c.moveTo(cx,cy-r);c.lineTo(cx,cy+r);
  c.strokeStyle='#0c1d38';c.lineWidth=1;c.stroke();

  var g=9.81;
  var bx=cx+(ay/g)*r*.82,by=cy+(ax/g)*r*.82;
  var dx=bx-cx,dy=by-cy,d=Math.sqrt(dx*dx+dy*dy);
  if(d>r*.88){bx=cx+dx/d*r*.88;by=cy+dy/d*r*.88;}

  var grd=c.createRadialGradient(bx,by,0,bx,by,18);
  grd.addColorStop(0,'rgba(0,230,118,.45)');
  grd.addColorStop(1,'rgba(0,230,118,0)');
  c.beginPath();c.arc(bx,by,18,0,Math.PI*2);
  c.fillStyle=grd;c.fill();

  c.beginPath();c.arc(bx,by,7,0,Math.PI*2);
  c.fillStyle='#00e676';c.fill();
  c.strokeStyle='#00ff9d';c.lineWidth=1.5;c.stroke();
}

// ── heart rate waveform ──────────────────────────────
function drawHR(finger){
  var w=195,h=58;
  var c=hrCtx;
  c.clearRect(0,0,w,h);
  c.fillStyle='rgba(0,0,0,.4)';c.fillRect(0,0,w,h);

  if(!finger||hrHistory.every(function(v){return v===0})){
    c.strokeStyle='#112030';c.lineWidth=1;
    c.beginPath();c.moveTo(0,h/2);c.lineTo(w,h/2);c.stroke();
    c.fillStyle='#1a3040';c.font='10px Courier New';
    c.textAlign='center';c.fillText('NO SIGNAL',w/2,h/2+4);
    return;
  }
  var step=w/hrHistory.length;
  var mx=Math.max.apply(null,hrHistory)||1;
  var mn=Math.min.apply(null,hrHistory);
  var rng=mx-mn||1;
  c.strokeStyle='#ff3d57';c.lineWidth=1.5;
  c.shadowColor='rgba(255,61,87,.5)';c.shadowBlur=7;
  c.beginPath();
  hrHistory.forEach(function(v,i){
    var x=i*step,y=h-((v-mn)/rng)*(h-8)-4;
    if(i===0)c.moveTo(x,y);else c.lineTo(x,y);
  });
  c.stroke();c.shadowBlur=0;
}

// ── arrow ────────────────────────────────────────────
function updateArrow(rel,state){
  var svg=document.getElementById('dirArrowSvg');
  var poly=document.getElementById('dirArrow');
  var sl=(state||'').toLowerCase();
  var col='#1a3050';
  if(sl.indexOf('forward')>=0)      col='#00ff9d';
  else if(sl.indexOf('right')>=0||sl.indexOf('left')>=0) col='#ffab00';
  else if(sl.indexOf('back')>=0)    col='#ff3d57';
  else if(sl.indexOf('arrived')>=0) col='#00e676';
  poly.setAttribute('fill',col);
  if(rel!==null) svg.style.transform='rotate('+rel+'deg)';
}

// ── nav state ────────────────────────────────────────
function updateNav(state,dist){
  var el=document.getElementById('navState');
  var sub=document.getElementById('navSub');
  var tag=document.getElementById('arrivedTag');
  var cls='ns-none',txt='AWAITING GPS',subTxt='';
  if(!state){cls='ns-none';txt='AWAITING GPS';}
  else if(state.indexOf('FORWARD')>=0){cls='ns-fwd';txt='&#9650; GO FORWARD';subTxt='STAY ON COURSE';}
  else if(state.indexOf('RIGHT')>=0)  {cls='ns-turn';txt='&#9654; TURN RIGHT';subTxt='ROTATE RIGHT';}
  else if(state.indexOf('LEFT')>=0)   {cls='ns-turn';txt='&#9664; TURN LEFT';subTxt='ROTATE LEFT';}
  else if(state.indexOf('BACK')>=0)   {cls='ns-back';txt='&#9660; TURN AROUND';subTxt='TARGET IS BEHIND YOU';}
  else if(state.indexOf('ARRIVED')>=0){cls='ns-arr';txt='&#10003; ARRIVED';subTxt='TARGET REACHED';}
  else if(state.indexOf('NO GPS')>=0) {cls='ns-none';txt='NO GPS FIX';subTxt='WAITING FOR LOCK';}
  else if(state.indexOf('WALK')>=0)   {cls='ns-none';txt='START WALKING';subTxt='NEED HEADING LOCK';}
  el.className='nav-state '+cls;
  el.innerHTML=txt;
  sub.textContent=subTxt;
  if(state&&state.indexOf('ARRIVED')>=0){tag.textContent='ARRIVED';tag.className='tag g';}
  else{tag.textContent='EN ROUTE';tag.className='tag a';}
}

// ── gyro bar ─────────────────────────────────────────
function setGBar(fillId,valId,val){
  var f=document.getElementById(fillId);
  var v=document.getElementById(valId);
  var mx=4.0;
  var pct=Math.min(Math.abs(val)/mx*50,50);
  if(val>=0){f.style.left='50%';f.style.right='auto';f.style.width=pct+'%';}
  else{f.style.right='50%';f.style.left='auto';f.style.width=pct+'%';}
  v.textContent=(val>=0?'+':'')+val.toFixed(3);
}

// ── sat bars ─────────────────────────────────────────
function buildSatBars(n){
  var c=document.getElementById('satBars');
  c.innerHTML='';
  for(var i=0;i<12;i++){
    var b=document.createElement('div');
    b.className='sat-bar'+(i<n?' lit':'');
    b.style.height=(8+i*12/12)+'px';
    c.appendChild(b);
  }
}

// ── RSSI segments ────────────────────────────────────
function buildRssiSegs(rssi){
  var c=document.getElementById('rssiSegs');
  c.innerHTML='';
  // rssi typically -120 to -40 dBm; map to 0-10
  var norm=Math.min(10,Math.max(0,Math.round((rssi+120)/8)));
  for(var i=0;i<10;i++){
    var s=document.createElement('div');
    s.className='rseg';
    s.style.height=(8+i*1.8)+'px';
    if(i<norm){
      if(norm>=7) s.className='rseg lit-g';
      else if(norm>=4) s.className='rseg lit-a';
      else s.className='rseg lit-r';
    }
    c.appendChild(s);
  }
}

// ── heading source badge ──────────────────────────────
function updateHdgBadge(src){
  var el=document.getElementById('hdgSrcBadge');
  var s=(src||'').trim()||'NONE';
  el.className='hsrc '+s;
  el.textContent='HDG: '+s;
}

// ── main update ──────────────────────────────────────
function updateUI(d){
  if(!d)return;
  lastPktAt=Date.now();
  prevPktMs=lastPktMs;
  lastPktMs=Date.now();

  var g=d.gps||{},imu=d.imu||{},nav=d.nav||{},hr=d.hr||{},meta=d.meta||{};
  var pkt=d.packetNum||0;

  // topbar
  document.getElementById('connDot').className='dot live';
  document.getElementById('connText').textContent='LIVE — '+(meta.node_name||'NODE');
  document.getElementById('topNode').textContent=meta.node_name||'--';
  document.getElementById('topPkt').textContent=pkt;
  document.getElementById('pktBig').textContent=pkt;
  document.getElementById('pktStat').textContent=pkt;
  document.getElementById('drPkts').textContent=pkt;

  // panic
  var panicBanner=document.getElementById('panicBanner');
  if(meta.panic===1){
    panicBanner.style.display='block';
    document.getElementById('panicNode').textContent=meta.node_name||'UNKNOWN';
  } else {
    panicBanner.style.display='none';
  }

  // GPS
  var gv=g.valid;
  document.getElementById('gLat').textContent=gv?(g.lat||0).toFixed(6):'--';
  document.getElementById('gLng').textContent=gv?(g.lng||0).toFixed(6):'--';
  document.getElementById('gAlt').textContent=gv?((g.alt||0).toFixed(1)+' m'):'--';
  document.getElementById('gSpd').textContent=gv?((g.speed||0).toFixed(1)+' km/h'):'--';
  var sats=g.satellites||0;
  document.getElementById('gSats').textContent=sats+' sats';
  buildSatBars(sats);
  var gpsTag=document.getElementById('gpsTag');
  gpsTag.textContent=gv?'VALID FIX':'NO FIX';
  gpsTag.className='tag '+(gv?'g':'r');

  // Base station panel
  var base = d.base || {};
  var baseLocked = base.locked || false;
  document.getElementById('baseLat').textContent = baseLocked ? (base.lat||0).toFixed(6) : '--';
  document.getElementById('baseLng').textContent = baseLocked ? (base.lng||0).toFixed(6) : '--';
  var bsTag = document.getElementById('baseLockTag');
  bsTag.textContent = baseLocked ? 'LOCKED' : 'CAPTURING';
  bsTag.className = 'tag ' + (baseLocked ? 'g' : 'a');
  var bsEl = document.getElementById('baseStatus');
  bsEl.textContent = baseLocked ? 'LOCKED' : 'CAPTURING';
  bsEl.className = 'gc-val ' + (baseLocked ? 'g' : 'a');
  var navActEl = document.getElementById('navActiveEl');
  navActEl.textContent = nav.active ? 'ACTIVE' : 'STANDBY';
  navActEl.className = 'gc-val ' + (nav.active ? 'g' : 'p');
  var dist = nav.distance || 0;
  document.getElementById('bsDist').textContent = dist.toFixed(1);
  document.getElementById('bsDist').style.color = dist < 5 ? 'var(--green)' : dist < 30 ? 'var(--amber)' : 'var(--red)';
  document.getElementById('bsBearing').textContent = (nav.bearing||0).toFixed(1) + '°';
  document.getElementById('bsRel').textContent = (nav.rel_bearing >= 0 ? '+' : '') + (nav.rel_bearing||0).toFixed(1) + '°';

  // IMU
  if(imu.valid){
    var ax=imu.accel_x||0,ay=imu.accel_y||0,az=imu.accel_z||0;
    document.getElementById('aX').textContent=ax.toFixed(2);
    document.getElementById('aY').textContent=ay.toFixed(2);
    document.getElementById('aZ').textContent=az.toFixed(2);
    setGBar('gbX','gvX',imu.gyro_x||0);
    setGBar('gbY','gvY',imu.gyro_y||0);
    setGBar('gbZ','gvZ',imu.gyro_z||0);
    document.getElementById('imuTemp').textContent=(imu.temp||0).toFixed(1)+' °C';
    document.getElementById('imuBias').textContent=(nav.bias_z!==undefined?nav.bias_z.toFixed(5):'--');
    drawTilt(ax,ay);
    document.getElementById('imuTag').className='tag g';
    document.getElementById('imuTag').textContent='ACTIVE';
  } else {
    document.getElementById('imuTag').className='tag r';
    document.getElementById('imuTag').textContent='INACTIVE';
  }

  // Navigation
  var rel  = nav.rel_bearing!==undefined ? nav.rel_bearing : null;
  var bear = nav.bearing!==undefined     ? nav.bearing     : null;
  var hdg  = nav.fused_heading!==undefined? nav.fused_heading:null;
  var dist = nav.distance!==undefined    ? nav.distance    : null;
  var state= nav.state||'';

  drawCompass(hdg,bear);
  updateArrow(rel,state);
  updateNav(state,dist);
  updateHdgBadge(nav.heading_src);

  document.getElementById('cvHdg').textContent  = hdg!==null  ? hdg.toFixed(1)+'°'  : '---°';
  document.getElementById('cvBear').textContent = bear!==null ? bear.toFixed(1)+'°' : '---°';
  document.getElementById('cvRel').textContent  = rel!==null  ? (rel>0?'+':'')+rel.toFixed(1)+'°' : '---°';
  document.getElementById('relBearLabel').textContent = rel!==null ? 'REL: '+(rel>0?'+':'')+rel.toFixed(1)+'°' : 'REL: --';

  if(dist!==null){
    var big=dist>1000?(dist/1000).toFixed(2)+' km':Math.round(dist)+'';
    document.getElementById('distNum').textContent=big;
    document.getElementById('distUnit').textContent=dist>1000?'KM TO BASE':'METRES TO BASE';
    var pct=Math.max(0,Math.min(100,(1-dist/25000)*100));
    document.getElementById('distBar').style.width=pct+'%';
  }
  document.getElementById('drState').textContent  = state.replace(/[<>*]/g,'').trim()||'--';
  document.getElementById('drBearing').textContent= bear!==null?bear.toFixed(1)+'°':'--';
  document.getElementById('drSpeed').textContent  = g.speed!==undefined?g.speed.toFixed(1)+' km/h':'--';
  document.getElementById('drAlt').textContent    = g.alt!==undefined?g.alt.toFixed(1)+' m':'--';

  // Heart rate
  if(hr.valid){
    var fin=hr.finger,bpm=hr.bpm||0;
    document.getElementById('hrNum').textContent=fin&&bpm>0?Math.round(bpm):'---';
    document.getElementById('hrNum').className='hr-bpm '+(fin&&bpm>0?'on':'off');
    document.getElementById('hrStatus').innerHTML=fin?'&#9632; FINGER DETECTED':'&#9632; NO FINGER';
    document.getElementById('hrStatus').className='hr-status '+(fin?'on':'off');
    document.getElementById('hrTag').className='tag '+(fin?'r':'a');
    document.getElementById('hrTag').textContent=fin?'ACTIVE':'STANDBY';
    hrHistory.push(fin&&bpm>0?bpm:0);
    hrHistory.shift();
    drawHR(fin);
  } else {
    document.getElementById('hrTag').className='tag r';
    document.getElementById('hrTag').textContent='NOT FOUND';
    document.getElementById('hrNum').textContent='N/A';
    document.getElementById('hrNum').className='hr-bpm off';
    document.getElementById('hrStatus').textContent='■ SENSOR NOT DETECTED';
    document.getElementById('hrStatus').className='hr-status off';
    drawHR(false);
  }

  // Node meta
  document.getElementById('metaName').textContent=meta.node_name||'--';
  document.getElementById('metaId').textContent=meta.node_id||'--';
  var rssiVal=meta.rssi||0;
  document.getElementById('metaRssi').textContent=rssiVal+' dBm';
  document.getElementById('statRssi').textContent=rssiVal+' dBm';
  buildRssiSegs(rssiVal);
  var panicEl=document.getElementById('metaPanic');
  if(meta.panic===1){panicEl.textContent='!! SOS !!';panicEl.className='gc-val r';}
  else{panicEl.textContent='OK';panicEl.className='gc-val g';}

  // Ticker
  var now=new Date();
  var upSec=Math.floor((Date.now()-startedAt)/1000);
  document.getElementById('tkTime').textContent=now.toLocaleTimeString();
  document.getElementById('tkGps').textContent=gv?g.lat.toFixed(4)+','+g.lng.toFixed(4):'NO FIX';
  document.getElementById('tkHdg').textContent=hdg!==null?hdg.toFixed(1)+'°':'--';
  document.getElementById('tkDist').textContent=dist!==null?(dist>1000?(dist/1000).toFixed(1)+'km':Math.round(dist)+'m'):'--';
  document.getElementById('tkHr').textContent=(hr.valid&&hr.finger&&hr.bpm>0)?Math.round(hr.bpm)+' BPM':'NO FINGER';
  document.getElementById('tkUp').textContent=Math.floor(upSec/60)+'m '+(upSec%60)+'s';
  document.getElementById('statUp').textContent=Math.floor(upSec/60)+'m '+(upSec%60)+'s';

  // Transport label
  var transport = d.transport || (nav.heading_src === 'LORA' ? 'LORA' : 'MESH');
  var tEl = document.getElementById('statTransport');
  tEl.textContent = transport;
  tEl.className   = 'dv ' + (transport === 'LORA' ? 'a' : 'p');

  // Interval estimate
  if(prevPktMs>0){
    var intv=Math.round((lastPktMs-prevPktMs)/100)/10;
    document.getElementById('statInterval').textContent=intv.toFixed(1)+' s';
  }

  // Raw JSON
  document.getElementById('rawJson').textContent=JSON.stringify(d,null,2);
}

// ── age ticker ───────────────────────────────────────
setInterval(function(){
  if(!lastPktAt)return;
  var age=(Date.now()-lastPktAt)/1000;
  document.getElementById('topAge').textContent=age.toFixed(1)+'s';
  document.getElementById('drAge').textContent=age.toFixed(1)+'s ago';
  if(age>8){
    document.getElementById('connDot').className='dot warn';
    document.getElementById('connText').textContent='STALE — '+age.toFixed(1)+'s';
  }
},500);

// ── poll ─────────────────────────────────────────────
setInterval(function(){
  fetch('/api/data',{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(updateUI)
    .catch(function(){
      document.getElementById('connDot').className='dot';
      document.getElementById('connText').textContent='DISCONNECTED';
    });
},2000);

// ── initial draw ─────────────────────────────────────
drawCompass(null,null);
drawTilt(0,0);
drawHR(false);
buildSatBars(0);
buildRssiSegs(-100);
</script>
</body>
</html>
)rawliteral";


// ════════════════════════════════════════════════════════════
//  HTTP HANDLERS
// ════════════════════════════════════════════════════════════
void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleApiData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", latestDataStr);
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
  mesh.setRoot(true);
  mesh.setContainsRoot(true);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onDroppedConnection(&droppedConnectionCallback);

  // server.begin() AFTER mesh.init() — mesh reconfigures WiFi first
  server.on("/",           handleRoot);
  server.on("/index.html", handleRoot);
  server.on("/api/data",   handleApiData);
  server.begin();

  Serial.println("[WiFi] Web server started");
  Serial.printf("[WiFi] AP SSID: %s\n", MESH_PREFIX);
  Serial.printf("[WiFi] AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("[WiFi] AP IP: %s\n",   WiFi.softAPIP().toString().c_str());
  Serial.printf("[WiFi] STA IP: %s\n",  WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] Dashboard: http://%s/\n", WiFi.softAPIP().toString().c_str());

  // LoRa RX — always active; receives from mesh nodes that fell back to LoRa
  loraReady = initLoRaRX();
  if (loraReady) {
    Serial.println("[LoRa] RX active — will receive packets from nodes in fallback mode");
  } else {
    Serial.println("[LoRa] Init failed — LoRa RX unavailable");
  }

  Serial.println("[READY] Mesh primary + LoRa RX fallback ready");
}


// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  mesh.update();
  cleanupNodes();
  server.handleClient();
  if (loraReady) processLoRaPackets();
}
