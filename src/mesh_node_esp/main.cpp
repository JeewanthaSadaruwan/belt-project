/*
 * ============================================================
 *  ESP32 MESH NODE — Belt Bridge Node (with LoRa fallback)
 *
 *  ROLE IN SYSTEM:
 *    Sits physically next to the Belt ESP32 and a Sensor Node.
 *    - Belt ESP32 sends navigation JSON over UART (GPIO16 RX)
 *    - Sensor Node (RPi/ESP) sends telemetry JSON over UART (GPIO13 RX)
 *    Combined data is forwarded over painlessMesh (ESP-NOW) and
 *    optionally LoRa as a parallel/redundant transport.
 *
 *  TRANSPORT DECISION FLAG:
 *    USE_ESP_NOW_ONLY true  → mesh/ESP-NOW only, LoRa is silent
 *    USE_ESP_NOW_ONLY false → mesh AND LoRa both transmit
 *
 *  WIRING:
 *    Belt ESP32 GPIO4  ──────►  This Node GPIO16 (Serial2 RX)  115200
 *    Sensor Node TX    ──────►  This Node GPIO13 (Serial1 RX)  230400
 *    Shared GND required on both UARTs
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
 *    Sensor UART RX  GPIO13 (Serial1 RX)
 *    Panic Button    GPIO4  (active LOW, internal pull-up)
 *    Status LED      GPIO2
 *
 *  MESH CONFIG:
 *    SSID     : BELT_BASE_9271_X9
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

// ── Transport strategy ─────────────────────────────────────────
// LoRa is ALWAYS initialised.
// At TX time:
//   • If mesh has at least one peer → send via mesh (primary)
//   • If mesh has no peers          → send via LoRa (automatic fallback)
//   • After LORA_FALLBACK_MS with no mesh → also LoRa even if mesh rejoins
#define LORA_FALLBACK_MS  8000   // switch to LoRa if mesh absent for 8 s

// ── Mesh config ───────────────────────────────────────────────
#define MESH_PREFIX   "BELT_BASE_9271_X9"
#define MESH_PASSWORD "12345678"
#define MESH_PORT     5555

// ── Belt UART (GPIO16 RX ← Belt ESP32 GPIO4 TX) ───────────────
#define BELT_UART_RX   16
#define BELT_UART_TX   17    // not used (one-way receive only)
#define BELT_UART_BAUD 115200

// ── Sensor Node UART (GPIO13 RX ← Sensor Node TX) ─────────────
#define SENSOR_UART_RX   13
#define SENSOR_UART_TX   15  // not used (one-way receive only)
#define SENSOR_UART_BAUD 230400

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
#define LORA_MAX_DATA  256

static uint16_t loraPacketCounter = 0;
static bool     loraReady         = false;

// ── Mesh objects ──────────────────────────────────────────────
Scheduler userScheduler;
painlessMesh mesh;

String        nodeName        = "TeamMember2";
bool          meshReady       = false;   // true while ≥1 peer is connected
unsigned long lastMeshConnMs  = 0;       // millis() of last mesh connection
bool          useLora         = false;   // runtime: true when falling back to LoRa

// ── Last data received from Belt ESP32 via UART ───────────────
struct BeltData {
  double lat     = 0.0;
  double lon     = 0.0;
  float  alt     = 0.0f;
  float  spd     = 0.0f;
  float  heading = 0.0f;
  float  dist    = 0.0f;
  float  rel     = 0.0f;
  float  temp    = 0.0f;
  float  hr      = 0.0f;
  float  ax      = 0.0f;
  float  ay      = 0.0f;
  float  az      = 0.0f;
  float  gx      = 0.0f;
  float  gy      = 0.0f;
  float  gz      = 0.0f;
  int    sats    = 0;
  bool   gpsOk   = false;
  String nav     = "";
  double baseLat = 0.0;
  double baseLng = 0.0;
  bool   baseOk  = false;
  bool   navOn   = false;
  bool   fresh   = false;
} beltData;

// ── Last data received from Sensor Node via UART ──────────────
struct SensorNodeData {
  String deviceID      = "";
  float  temp          = 0.0f;
  float  thermalMin    = 0.0f;
  float  thermalMax    = 0.0f;
  float  thermalAvg    = 0.0f;
  float  centerTemp    = 0.0f;
  float  co2           = 0.0f;
  int    co            = 0;
  String victimMAC     = "none";
  int    victimRSSI    = 0;
  float  victimDist    = 0.0f;
  String victimStatus  = "NO VICTIM";
  String localAlert    = "SAFE";
  String finalAlert    = "SAFE";
  bool   buzzer        = false;
  bool   tempHigh      = false;
  bool   tempCritical  = false;
  bool   coHigh        = false;
  bool   coCritical    = false;
  bool   co2High       = false;
  bool   co2Critical   = false;
  bool   hasMlxFrame   = false;
  int    mlxWidth      = 0;
  int    mlxHeight     = 0;
  bool   fresh         = false;
} sensorData;


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

  if (!LoRa.beginPacket()) return false;
  LoRa.write(buffer, 4 + dataLen);
  // endPacket(true) = async/non-blocking: puts radio in TX mode and returns
  // immediately without polling TX_DONE. Polling with yield() lets the mesh
  // FreeRTOS task interfere with SPI reads, causing an infinite hang.
  // At SF7/BW125kHz the packet is off the air in <500ms; next TX is 5s away.
  LoRa.endPacket(true);
  loraPacketCounter++;
  return true;
}


// ════════════════════════════════════════════════════════════
//  BELT UART RECEIVE
// ════════════════════════════════════════════════════════════
void readBeltUART() {
  static String buf = "";
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        beltData.lat     = doc["lat"]  | 0.0;
        beltData.lon     = doc["lon"]  | 0.0;
        beltData.alt     = doc["alt"]  | 0.0f;
        beltData.spd     = doc["spd"]  | 0.0f;
        beltData.heading = doc["hdg"]  | 0.0f;
        beltData.dist    = doc["dist"] | 0.0f;
        beltData.rel     = doc["rel"]  | 0.0f;
        beltData.temp    = doc["temp"] | 0.0f;
        beltData.hr      = doc["hr"]   | 0.0f;
        beltData.ax      = doc["ax"]   | 0.0f;
        beltData.ay      = doc["ay"]   | 0.0f;
        beltData.az      = doc["az"]   | 0.0f;
        beltData.gx      = doc["gx"]   | 0.0f;
        beltData.gy      = doc["gy"]   | 0.0f;
        beltData.gz      = doc["gz"]   | 0.0f;
        beltData.sats    = doc["sats"] | 0;
        beltData.gpsOk   = (doc["gps"]     | 0) == 1;
        beltData.nav     = doc["nav"]      | "";
        beltData.baseLat = doc["base_lat"] | 0.0;
        beltData.baseLng = doc["base_lng"] | 0.0;
        beltData.baseOk  = (doc["base_ok"] | 0) == 1;
        beltData.navOn   = (doc["nav_on"]  | 0) == 1;
        beltData.fresh   = true;
        // Pretty-print is done in sendMessage() so belt + sensor data
        // are always printed together when we actually transmit.
      } else {
        Serial.println("[BELT] JSON parse error — raw: " + buf);
      }
      buf = "";
    } else {
      buf += c;
      if (buf.length() > 600) buf = "";  // overflow guard
    }
  }
}


// ════════════════════════════════════════════════════════════
//  SENSOR NODE UART RECEIVE  (Serial1 GPIO13 RX @ 230400)
// ════════════════════════════════════════════════════════════
void readSensorUART() {
  static String buf = "";
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      buf.trim();
      if (buf.length() == 0) { buf = ""; return; }

      StaticJsonDocument<4096> doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        sensorData.deviceID     = doc["device_id"]       | "";
        sensorData.temp         = doc["temp"]            | 0.0f;
        sensorData.thermalMin   = doc["thermal_min"]     | 0.0f;
        sensorData.thermalMax   = doc["thermal_max"]     | 0.0f;
        sensorData.thermalAvg   = doc["thermal_avg"]     | 0.0f;
        sensorData.centerTemp   = doc["center_temp"]     | 0.0f;
        sensorData.co2          = doc["co2"]             | 0.0f;
        sensorData.co           = doc["co"]              | 0;
        sensorData.victimMAC    = doc["victim_mac"]      | "none";
        sensorData.victimRSSI   = doc["victim_rssi"]     | 0;
        sensorData.victimDist   = doc["victim_distance"] | 0.0f;
        sensorData.victimStatus = doc["victim_status"]   | "NO VICTIM";
        sensorData.localAlert   = doc["local_alert"]     | "SAFE";
        sensorData.finalAlert   = doc["final_alert"]     | "SAFE";
        sensorData.buzzer       = doc["buzzer"]          | false;
        sensorData.tempHigh     = doc["temp_high"]       | false;
        sensorData.tempCritical = doc["temp_critical"]   | false;
        sensorData.coHigh       = doc["co_high"]         | false;
        sensorData.coCritical   = doc["co_critical"]     | false;
        sensorData.co2High      = doc["co2_high"]        | false;
        sensorData.co2Critical  = doc["co2_critical"]    | false;
        sensorData.hasMlxFrame  = doc["has_mlx_frame"]   | false;
        sensorData.mlxWidth     = doc["mlx_width"]       | 0;
        sensorData.mlxHeight    = doc["mlx_height"]      | 0;
        sensorData.fresh        = true;
      } else {
        Serial.println("[SENSOR] JSON parse error — raw: " + buf);
      }
      buf = "";
    } else {
      buf += c;
      if (buf.length() > 4200) buf = "";  // overflow guard
    }
  }
}


// ════════════════════════════════════════════════════════════
//  COMBINED PRETTY PRINT
// ════════════════════════════════════════════════════════════
static uint32_t printCount = 0;

void printCombinedData(int panic, bool meshOk, bool loraOk, bool hasPeers, bool beltFresh, bool sensorFresh) {
  printCount++;
  Serial.println();
  Serial.println("============================================================");
  Serial.printf( " PKT #%-4u  |  Nav: %-12s  Panic: %s\n",
    printCount, beltData.nav.c_str(), panic ? "YES !!!" : "NO");
  Serial.println("------------------------------------------------------------");

  // ── Belt / GPS ──────────────────────────────────────────
  Serial.println(" [BELT DATA]");
  if (beltData.gpsOk) {
    Serial.printf("  GPS    | %.6f, %.6f  Alt:%.1fm  Spd:%.1fkm/h  Sats:%d\n",
      beltData.lat, beltData.lon, beltData.alt, beltData.spd, beltData.sats);
  } else {
    Serial.printf("  GPS    | NO FIX  Sats:%d\n", beltData.sats);
  }
  Serial.printf("  HDG    | %.1f deg   Dist:%.1fm   Rel:%+.1f deg\n",
    beltData.heading, beltData.dist, beltData.rel);
  Serial.printf("  BASE   | %.6f, %.6f  [%s]   NAV: %s\n",
    beltData.baseLat, beltData.baseLng,
    beltData.baseOk ? "LOCKED" : "CAPTURING...",
    beltData.navOn  ? "ACTIVE" : (beltData.baseOk ? "STANDBY" : "AWAITING BASE"));
  Serial.printf("  IMU    | Ax:%.2f Ay:%.2f Az:%.2f  Gx:%.3f Gy:%.3f Gz:%.3f\n",
    beltData.ax, beltData.ay, beltData.az,
    beltData.gx, beltData.gy, beltData.gz);
  Serial.printf("  TEMP   | %.1f C   HR: %.0f BPM   %s\n",
    beltData.temp, beltData.hr,
    beltFresh ? "(fresh)" : "(stale — no new belt packet)");

  Serial.println("------------------------------------------------------------");

  // ── Sensor Node ─────────────────────────────────────────
  Serial.println(" [SENSOR NODE DATA]");
  if (!sensorFresh) {
    Serial.println("  (no sensor packet received yet — all zeros)");
  }
  if (sensorData.deviceID.length() > 0)
    Serial.printf("  Device | %s\n", sensorData.deviceID.c_str());

  Serial.printf("  THERMAL| Amb:%.1fC  Min:%.1f  Max:%.1f  Avg:%.1f  Center:%.1f\n",
    sensorData.temp, sensorData.thermalMin,
    sensorData.thermalMax, sensorData.thermalAvg, sensorData.centerTemp);
  Serial.printf("  GAS    | CO2: %.0f ppm   CO raw: %d\n",
    sensorData.co2, sensorData.co);
  Serial.printf("  VICTIM | Status: %-12s  RSSI: %d dBm  Dist: %.1fm  MAC: %s\n",
    sensorData.victimStatus.c_str(), sensorData.victimRSSI,
    sensorData.victimDist, sensorData.victimMAC.c_str());
  Serial.printf("  ALERTS | Local: %-10s  Final: %-10s  Buzzer: %s\n",
    sensorData.localAlert.c_str(), sensorData.finalAlert.c_str(),
    sensorData.buzzer ? "ON  !!!" : "off");
  Serial.printf("  FLAGS  | TempH:%s TempC:%s  COH:%s COC:%s  CO2H:%s CO2C:%s\n",
    sensorData.tempHigh     ? "Y" : "N",
    sensorData.tempCritical ? "Y" : "N",
    sensorData.coHigh       ? "Y" : "N",
    sensorData.coCritical   ? "Y" : "N",
    sensorData.co2High      ? "Y" : "N",
    sensorData.co2Critical  ? "Y" : "N");
  if (sensorData.hasMlxFrame)
    Serial.printf("  MLX    | %dx%d frame received\n",
      sensorData.mlxWidth, sensorData.mlxHeight);

  Serial.println("------------------------------------------------------------");
  Serial.printf( "  TRANSPORT | Mesh: %-4s   LoRa: %-4s   Mode: %s\n",
    meshOk  ? "OK"  : (hasPeers ? "FAIL" : "----"),
    loraOk  ? "OK"  : ((!meshOk && loraReady) ? "FAIL" : "----"),
    meshOk  ? "MESH (primary)" : (loraOk ? "LORA (fallback)" : (!loraReady ? "NO LORA HW" : "ALL TX FAILED")));
  Serial.println("============================================================");
}


// ════════════════════════════════════════════════════════════
//  SEND — mesh broadcast + optional LoRa TX
// ════════════════════════════════════════════════════════════
void sendMessage() {
  // Run regardless of mesh state — LoRa fallback and pretty-print still needed
  int rssi  = WiFi.RSSI();
  int panic = (digitalRead(PANIC_BUTTON) == LOW) ? 1 : 0;

  // Closest upstream mesh node
  uint32_t parentId = 0;
  SimpleList<uint32_t> list = mesh.getNodeList();
  if (list.size() > 0) parentId = list.front();

  if (!beltData.fresh) {
    Serial.println("[WARN] No new belt data since last TX — using last known values");
  }
  if (!sensorData.fresh) {
    Serial.println("[WARN] No sensor node data received — sensor fields will be 0");
  }
  // Save fresh flags for the print, then clear them
  bool beltWasFresh   = beltData.fresh;
  bool sensorWasFresh = sensorData.fresh;
  beltData.fresh   = false;
  sensorData.fresh = false;

  // ── Decide transport ────────────────────────────────────────
  // Simple: try mesh if peers exist, fall back to LoRa immediately if not.
  bool hasPeers = (mesh.getNodeList().size() > 0);

  // ── 1. painlessMesh broadcast (ESP-NOW) ────────────────────
  String meshMsg = nodeName +
    " Parent:"     + String(parentId) +
    " Nav:"        + beltData.nav +
    " Dist:"       + String(beltData.dist, 1) +
    " Hdg:"        + String(beltData.heading, 1) +
    " Rel:"        + String(beltData.rel, 1) +
    " Lat:"        + String(beltData.lat, 6) +
    " Lon:"        + String(beltData.lon, 6) +
    " Alt:"        + String(beltData.alt, 1) +
    " Spd:"        + String(beltData.spd, 1) +
    " BeltTemp:"   + String(beltData.temp, 1) +
    " HR:"         + String(beltData.hr, 1) +
    " Sats:"       + String(beltData.sats) +
    " GPS:"        + String(beltData.gpsOk ? 1 : 0) +
    " BaseLat:"    + String(beltData.baseLat, 6) +
    " BaseLng:"    + String(beltData.baseLng, 6) +
    " BaseOk:"     + String(beltData.baseOk ? 1 : 0) +
    " NavOn:"      + String(beltData.navOn  ? 1 : 0) +
    " Ax:"         + String(beltData.ax, 2) +
    " Ay:"         + String(beltData.ay, 2) +
    " Az:"         + String(beltData.az, 2) +
    " Gx:"         + String(beltData.gx, 3) +
    " Gy:"         + String(beltData.gy, 3) +
    " Gz:"         + String(beltData.gz, 3) +
    " AmbTemp:"    + String(sensorData.temp, 1) +
    " ThermalMax:" + String(sensorData.thermalMax, 1) +
    " ThermalAvg:" + String(sensorData.thermalAvg, 1) +
    " CO2:"        + String(sensorData.co2, 0) +
    " CO:"         + String(sensorData.co) +
    " VictimSt:"   + sensorData.victimStatus +
    " VictimDist:" + String(sensorData.victimDist, 1) +
    " Alert:"      + sensorData.finalAlert +
    " Buzzer:"     + String(sensorData.buzzer ? 1 : 0) +
    " Panic:"      + String(panic) +
    " RSSI:"       + String(rssi);

  bool meshOk  = false;
  bool loraOk  = false;

  if (hasPeers) {
    // ── Primary: mesh broadcast ───────────────────────────────
    meshOk = mesh.sendBroadcast(meshMsg);
    if (meshOk) {
      blinkLED(2, 50, 50);   // 2 quick blinks = mesh TX OK
    } else {
      Serial.println("[MESH TX] sendBroadcast returned false");
    }
  } else {
    Serial.println("[MESH TX] No peers — falling back to LoRa");
  }

  // ── 2. LoRa fallback if mesh unavailable or TX failed ──────
  if (!meshOk && loraReady) {
    char loraPayload[LORA_MAX_DATA];
    snprintf(loraPayload, sizeof(loraPayload),
      "name=%s,lat=%.6f,lon=%.6f,hdg=%.1f,dist=%.1f,rel=%.1f,"
      "btemp=%.1f,sats=%d,gps=%d,"
      "atemp=%.1f,thmax=%.1f,co2=%.0f,co=%d,"
      "vstatus=%s,vdist=%.1f,alert=%s,buzzer=%d,panic=%d,rssi=%d,"
      "blat=%.6f,blng=%.6f,bok=%d,nav=%d,navst=%s",
      nodeName.c_str(),
      beltData.lat, beltData.lon,
      beltData.heading, beltData.dist, beltData.rel,
      beltData.temp, beltData.sats, beltData.gpsOk ? 1 : 0,
      sensorData.temp, sensorData.thermalMax,
      sensorData.co2, sensorData.co,
      sensorData.victimStatus.c_str(), sensorData.victimDist,
      sensorData.finalAlert.c_str(),
      sensorData.buzzer ? 1 : 0,
      panic, rssi,
      beltData.baseLat, beltData.baseLng,
      beltData.baseOk ? 1 : 0, beltData.navOn ? 1 : 0,
      beltData.nav.length() ? beltData.nav.c_str() : "STANDBY");

    loraOk = sendLoRaPacketRaw(LORA_PKT_DATA, loraPayload);
    if (loraOk) {
      blinkLED(1, 80, 0);
      Serial.printf("[LoRa TX] #%04u  OK\n", loraPacketCounter - 1);
    } else {
      blinkLED(3, 80, 80);
      Serial.printf("[LoRa TX] #%04u  FAIL\n", loraPacketCounter);
    }
  } else if (!meshOk && !loraReady) {
    Serial.println("[TX] No transport available — mesh offline, LoRa not ready");
  }

  // ── 3. Pretty-print combined data ──────────────────────────
  printCombinedData(panic, meshOk, loraOk, hasPeers, beltWasFresh, sensorWasFresh);
}

Task taskSend(5000, TASK_FOREVER, &sendMessage);

void newConnectionCallback(uint32_t nodeId) {
  meshReady      = true;
  useLora        = false;
  // NOTE: lastMeshConnMs is NOT updated here — it is only updated when
  // a mesh TX actually succeeds, so spurious brief scan contacts during
  // startup don't keep resetting the LoRa fallback timer.
  Serial.printf("[MESH] Connected: node %u — switching to MESH transport\n", nodeId);
}

void droppedConnectionCallback(uint32_t nodeId) {
  meshReady = (mesh.getNodeList().size() > 0);
  if (!meshReady) {
    Serial.printf("[MESH] Lost node %u — no peers left, LoRa fallback active\n", nodeId);
    useLora = loraReady;
  } else {
    Serial.printf("[MESH] Lost node %u — still have peers, staying on mesh\n", nodeId);
  }
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
  Serial.printf("[BELT]   Serial2 RX ready  GPIO%d ← Belt ESP32 GPIO4 @ %d baud\n",
    BELT_UART_RX, BELT_UART_BAUD);

  // Wired UART from Sensor Node (RPi / sensor ESP)
  Serial1.begin(SENSOR_UART_BAUD, SERIAL_8N1, SENSOR_UART_RX, SENSOR_UART_TX);
  Serial.printf("[SENSOR] Serial1 RX ready  GPIO%d ← Sensor Node TX  @ %d baud\n",
    SENSOR_UART_RX, SENSOR_UART_BAUD);

  // Mesh — STA mode: base station exposes the mesh AP, node connects to it
  // Must come BEFORE LoRa init — mesh.init() reconfigures WiFi/SPI peripheral
  // state and would corrupt a previously-initialised LoRa module.
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_STA);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onDroppedConnection(&droppedConnectionCallback);

  // LoRa — init AFTER mesh so SPI is in its final state
  loraReady = initLoRa();
  if (loraReady) {
    Serial.println("[LoRa] Ready — will activate automatically if mesh is unreachable");
  } else {
    Serial.println("[LoRa] Init failed — LoRa fallback unavailable");
  }

  userScheduler.addTask(taskSend);
  taskSend.enable();

  Serial.printf("[MESH] Scanning for mesh: %s\n", MESH_PREFIX);
  Serial.println("[MESH] Transport: mesh when peers present, LoRa immediately if not");
}


// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  readBeltUART();    // drain Belt ESP32 UART buffer continuously
  readSensorUART();  // drain Sensor Node UART buffer continuously
  mesh.update();
}
