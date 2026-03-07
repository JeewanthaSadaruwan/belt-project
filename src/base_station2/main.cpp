/*
 * BASE STATION 2 - WiFi HTTP Receiver + Dashboard
 * Receives sensor data from Belt 2 via HTTP POST
 * Serves dashboard with inline HTML (no filesystem needed)
 * Dashboard polls /api/data every 2 seconds
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>

#define AP_SSID     "BeltStation"
#define AP_PASSWORD "belt12345"
#define STATUS_LED  2

WebServer server(80);
DNSServer dnsServer;
String latestDataStr = "{}";
int packetsReceived = 0;
unsigned long lastHttpLog = 0;
unsigned long lastPacketMs = 0;

// =====================================================
//  INLINE DASHBOARD HTML
// =====================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Belt Station 2 Dashboard</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:#0b0f1f;
  --bg2:#0a1224;
  --panel:#10172b;
  --panel2:#0b1328;
  --line:#1e2a44;
  --accent:#19c2ff;
  --accent2:#7c3aed;
  --good:#22c55e;
  --warn:#f59e0b;
  --bad:#ef4444;
  --text:#e6ebf5;
  --muted:#93a0ba;
}
body{
  font-family:"Bahnschrift","Gill Sans MT","Trebuchet MS",sans-serif;
  background:
    radial-gradient(800px 400px at 10% -10%, rgba(25,194,255,.18), transparent 60%),
    radial-gradient(900px 500px at 100% 0%, rgba(124,58,237,.18), transparent 55%),
    linear-gradient(180deg,var(--bg) 0%, var(--bg2) 100%);
  color:var(--text);
  min-height:100vh;
}
.topbar{
  position:sticky;
  top:0;
  z-index:2;
  padding:18px 24px;
  border-bottom:1px solid var(--line);
  display:flex;
  gap:16px;
  align-items:center;
  justify-content:space-between;
  background:linear-gradient(180deg, rgba(10,18,36,.98) 0%, rgba(11,15,31,.92) 100%);
  backdrop-filter: blur(6px);
}
.brand{
  display:flex;
  flex-direction:column;
  gap:4px;
}
.brand .title{
  font-size:1.4rem;
  letter-spacing:1.5px;
  text-transform:uppercase;
}
.brand .subtitle{
  font-size:0.85rem;
  color:var(--muted);
  letter-spacing:.6px;
}
.status-pill{
  display:flex;
  align-items:center;
  gap:10px;
  padding:8px 14px;
  border:1px solid var(--line);
  border-radius:22px;
  background:#0b1226;
  font-size:0.85rem;
  box-shadow:0 0 0 1px rgba(25,194,255,.08), inset 0 0 20px rgba(25,194,255,.04);
}
.dot{
  width:10px;height:10px;border-radius:50%;
  background:var(--bad);
}
.dot.on{background:var(--good);box-shadow:0 0 10px rgba(34,197,94,.6)}
.dot.warn{background:var(--warn);box-shadow:0 0 10px rgba(245,158,11,.6)}
.wrap{max-width:1250px;margin:0 auto;padding:20px}
.layout{
  display:grid;
  grid-template-columns:1.1fr 1fr 1fr;
  grid-template-areas:
    "status gps gps"
    "accel gyro system"
    "raw raw raw";
  gap:16px;
}
.card{
  background:linear-gradient(180deg,var(--panel) 0%, var(--panel2) 100%);
  border:1px solid var(--line);
  border-radius:18px;
  padding:16px;
  box-shadow:0 10px 30px rgba(5,10,25,.35);
}
.card-head{
  display:flex;
  align-items:center;
  justify-content:space-between;
  padding-bottom:12px;
  border-bottom:1px solid rgba(30,42,68,.7);
  margin-bottom:12px;
}
.eyebrow{
  font-size:0.72rem;
  color:var(--muted);
  letter-spacing:1.2px;
  text-transform:uppercase;
}
.card-title{
  font-size:1.05rem;
  letter-spacing:1px;
  text-transform:uppercase;
}
.chip{
  font-size:0.8rem;
  color:var(--muted);
  padding:4px 10px;
  border-radius:999px;
  border:1px solid var(--line);
  background:#0b1226;
}
.chip .value{margin:0;font-size:0.8rem}
.stat-grid{
  display:grid;
  grid-template-columns:repeat(2,1fr);
  gap:12px;
}
.stat{
  background:linear-gradient(180deg, rgba(11,18,38,.8), rgba(11,18,38,.4));
  border:1px solid var(--line);
  border-radius:14px;
  padding:12px;
}
.label{color:var(--muted);font-size:0.8rem}
.value{
  margin-top:6px;
  font-family:"Cascadia Mono","Consolas","Lucida Console",monospace;
  font-weight:700;
  font-size:1.05rem;
}
.value.good{color:var(--good)}
.value.warn{color:var(--warn)}
.value.bad{color:var(--bad)}
.big{font-size:1.9rem;letter-spacing:1px}
.kv{
  display:flex;
  justify-content:space-between;
  padding:10px 6px;
  border-bottom:1px dashed rgba(30,42,68,.7);
  font-size:0.95rem;
}
.kv:last-child{border-bottom:none}
.axis{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:10px;
}
.box{
  background:#0b1226;
  border:1px solid var(--line);
  border-radius:14px;
  text-align:center;
  padding:14px 10px;
  box-shadow:inset 0 0 20px rgba(25,194,255,.05);
}
.axis-label{
  font-size:0.72rem;
  color:var(--muted);
  text-transform:uppercase;
  letter-spacing:1px;
}
.axis-val{
  font-size:1.25rem;
  margin-top:8px;
  font-family:"Cascadia Mono","Consolas","Lucida Console",monospace;
}
.raw{
  background:#0b1226;
  border:1px solid var(--line);
  border-radius:12px;
  padding:12px;
  font-family:"Cascadia Mono","Consolas","Lucida Console",monospace;
  font-size:0.82rem;
  color:#c8d3ee;
  max-height:240px;
  overflow:auto;
  white-space:pre-wrap;
}
.status{grid-area:status}
.gps{grid-area:gps}
.accel{grid-area:accel}
.gyro{grid-area:gyro}
.system{grid-area:system}
.rawcard{grid-area:raw}
.footer{
  text-align:center;
  padding:18px;
  color:#6f7b92;
  font-size:0.8rem;
}
@media (max-width:1100px){
  .layout{
    grid-template-columns:1fr 1fr;
    grid-template-areas:
      "status gps"
      "accel gyro"
      "system system"
      "raw raw";
  }
}
@media (max-width:780px){
  .topbar{flex-direction:column;align-items:flex-start}
  .layout{
    grid-template-columns:1fr;
    grid-template-areas:
      "status"
      "gps"
      "accel"
      "gyro"
      "system"
      "raw";
  }
}
</style>
</head>
<body>
<header class="topbar">
  <div class="brand">
    <div class="title">Belt Station 2</div>
    <div class="subtitle">Field telemetry console</div>
  </div>
  <div class="status-pill">
    <div class="dot" id="connDot"></div>
    <span id="connText">Waiting for packets...</span>
  </div>
</header>
<main class="wrap">
  <div class="layout">
    <section class="card status">
      <div class="card-head">
        <div>
          <div class="eyebrow">Link Status</div>
          <div class="card-title">Telemetry Link</div>
        </div>
        <div class="chip">Live</div>
      </div>
      <div class="stat-grid">
        <div class="stat">
          <div class="label">Protocol</div>
          <div class="value" id="protoText">HTTP Polling</div>
        </div>
        <div class="stat">
          <div class="label">Packets</div>
          <div class="value big" id="pkt">0</div>
        </div>
        <div class="stat">
          <div class="label">Last Update</div>
          <div class="value" id="upd">--</div>
        </div>
        <div class="stat">
          <div class="label">Packet Age</div>
          <div class="value" id="age">--</div>
        </div>
      </div>
    </section>
    <section class="card gps">
      <div class="card-head">
        <div>
          <div class="eyebrow">GPS</div>
          <div class="card-title">Position & Fix</div>
        </div>
        <div class="chip"><span id="gpsStatus" class="value">--</span></div>
      </div>
      <div class="kv"><span class="label">Latitude</span><span class="value" id="lat">--</span></div>
      <div class="kv"><span class="label">Longitude</span><span class="value" id="lng">--</span></div>
      <div class="kv"><span class="label">Altitude</span><span class="value" id="alt">--</span></div>
      <div class="kv"><span class="label">Speed</span><span class="value" id="spd">--</span></div>
      <div class="kv"><span class="label">Satellites</span><span class="value" id="sat">--</span></div>
    </section>
    <section class="card accel">
      <div class="card-head">
        <div>
          <div class="eyebrow">Accelerometer</div>
          <div class="card-title">m/s²</div>
        </div>
        <div class="chip">IMU</div>
      </div>
      <div class="axis">
        <div class="box"><div class="axis-label">X</div><div class="axis-val" id="ax">--</div></div>
        <div class="box"><div class="axis-label">Y</div><div class="axis-val" id="ay">--</div></div>
        <div class="box"><div class="axis-label">Z</div><div class="axis-val" id="az">--</div></div>
      </div>
    </section>
    <section class="card gyro">
      <div class="card-head">
        <div>
          <div class="eyebrow">Gyroscope</div>
          <div class="card-title">rad/s</div>
        </div>
        <div class="chip">IMU</div>
      </div>
      <div class="axis">
        <div class="box"><div class="axis-label">X</div><div class="axis-val" id="gx">--</div></div>
        <div class="box"><div class="axis-label">Y</div><div class="axis-val" id="gy">--</div></div>
        <div class="box"><div class="axis-label">Z</div><div class="axis-val" id="gz">--</div></div>
      </div>
    </section>
    <section class="card system">
      <div class="card-head">
        <div>
          <div class="eyebrow">System</div>
          <div class="card-title">Device Health</div>
        </div>
        <div class="chip">IMU</div>
      </div>
      <div class="kv"><span class="label">IMU Status</span><span class="value" id="imuSt">--</span></div>
      <div class="kv"><span class="label">Temperature</span><span class="value" id="temp">--</span></div>
    </section>
    <section class="card rawcard">
      <div class="card-head">
        <div>
          <div class="eyebrow">Raw</div>
          <div class="card-title">JSON Stream</div>
        </div>
        <div class="chip">/api/data</div>
      </div>
      <div class="raw" id="raw">{}</div>
    </section>
  </div>
</main>
<div class="footer">HTTP receiver + dashboard (no internet required)</div>
<script>
var connDot = document.getElementById('connDot');
var connText = document.getElementById('connText');
var protoText = document.getElementById('protoText');
var lastPacketAt = 0;
var polling = false;

function setStatus(state, text){
  connDot.className = 'dot' + (state==='on' ? ' on' : state==='warn' ? ' warn' : '');
  connText.textContent = text;
}

function fmt(n, digits){
  if(typeof n !== 'number' || !isFinite(n)) return '--';
  return n.toFixed(digits);
}

function updateUI(d){
  if(!d || !d.gps || !d.imu){
    if(lastPacketAt === 0) setStatus('warn','Waiting for packets...');
    return;
  }
  lastPacketAt = Date.now();
  var g = d.gps;
  var m = d.imu;

  document.getElementById('gpsStatus').textContent = g.valid ? 'Valid Fix' : 'No Fix';
  document.getElementById('gpsStatus').className = 'value ' + (g.valid ? 'good' : 'bad');
  document.getElementById('lat').textContent = fmt(g.lat, 6);
  document.getElementById('lng').textContent = fmt(g.lng, 6);
  document.getElementById('alt').textContent = fmt(g.alt, 1) + ' m';
  document.getElementById('spd').textContent = fmt(g.speed, 1) + ' km/h';
  document.getElementById('sat').textContent = (typeof g.satellites === 'number') ? g.satellites : '--';
  var satClass = g.satellites >= 6 ? 'good' : g.satellites >= 4 ? 'warn' : 'bad';
  document.getElementById('sat').className = 'value ' + satClass;

  document.getElementById('ax').textContent = fmt(m.accel_x, 2);
  document.getElementById('ay').textContent = fmt(m.accel_y, 2);
  document.getElementById('az').textContent = fmt(m.accel_z, 2);
  document.getElementById('gx').textContent = fmt(m.gyro_x, 3);
  document.getElementById('gy').textContent = fmt(m.gyro_y, 3);
  document.getElementById('gz').textContent = fmt(m.gyro_z, 3);
  document.getElementById('temp').textContent = fmt(m.temp, 1) + ' C';
  document.getElementById('imuSt').textContent = m.valid ? 'Active' : 'Inactive';
  document.getElementById('imuSt').className = 'value ' + (m.valid ? 'good' : 'bad');

  document.getElementById('pkt').textContent = d.packetNum || 0;
  document.getElementById('upd').textContent = new Date().toLocaleTimeString();
  document.getElementById('raw').textContent = JSON.stringify(d, null, 2);

  setStatus('on', 'Live - Packet #' + (d.packetNum || 0));
}

function startPolling(){
  if(polling) return;
  polling = true;
  protoText.textContent = 'HTTP Polling';
  setInterval(function(){
    fetch('/api/data', {cache:'no-store'})
      .then(function(r){return r.json()})
      .then(updateUI)
      .catch(function(){
        setStatus('warn','Disconnected - polling failed');
      });
  }, 2000);
}

setInterval(function(){
  if(lastPacketAt === 0) return;
  var age = (Date.now() - lastPacketAt) / 1000;
  document.getElementById('age').textContent = age.toFixed(1) + ' s';
  if(age > 5) setStatus('warn','Stale - last packet ' + age.toFixed(1) + 's ago');
}, 500);

startPolling();
</script>
</body>
</html>
)rawliteral";

// =====================================================
//  HTTP HELPERS
// =====================================================
const char* httpMethodName(HTTPMethod method) {
  switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_PATCH: return "PATCH";
    case HTTP_DELETE: return "DELETE";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_HEAD: return "HEAD";
    default: return "OTHER";
  }
}

void logHttpRequest() {
  // Throttle logs to avoid flooding
  if (millis() - lastHttpLog > 500) {
    Serial.printf("[HTTP] %s %s\n", httpMethodName(server.method()), server.uri().c_str());
    lastHttpLog = millis();
  }
}

void sendDashboard() {
  logHttpRequest();
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

bool updateFromJson(const String &body) {
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[HTTP] JSON parse error: %s\n", err.c_str());
    return false;
  }

  packetsReceived++;
  doc["timestamp"] = millis();
  doc["packetNum"] = packetsReceived;

  latestDataStr = "";
  serializeJson(doc, latestDataStr);
  lastPacketMs = millis();
  digitalWrite(STATUS_LED, HIGH);
  return true;
}

void sendApiData() {
  logHttpRequest();
  // CORS + no-cache for debugging from any browser
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");

  if (server.method() == HTTP_OPTIONS) {
    server.send(204, "text/plain", "");
    return;
  }

  if (server.method() == HTTP_GET) {
    server.send(200, "application/json", latestDataStr);
    return;
  }

  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    bool ok = updateFromJson(body);
    if (ok) {
      Serial.printf("[HTTP] Packet #%d (%d bytes)\n", packetsReceived, body.length());
      server.send(200, "application/json", latestDataStr);
    } else {
      server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
    }
    return;
  }

  server.send(405, "text/plain", "Method Not Allowed");
}

// =====================================================
//  SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(2000);  // Give USB CDC time to connect
  // Wait for USB serial on ESP32-S3 (up to 5 seconds)
  unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 5000)) { delay(10); }
  
  pinMode(STATUS_LED, OUTPUT);

  Serial.println("\n\n=== BASE STATION 2 ===");
  Serial.println("USB Serial connected!");

  // WiFi AP+STA mode (AP for dashboard, STA available if needed)
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, 1, 0, 4);  // Channel 1, not hidden
  Serial.printf("AP start: %s\n", apOk ? "OK" : "FAIL");
  Serial.printf("AP IP: %s  SSID: %s  Pass: %s  Channel: 1\n",
    WiFi.softAPIP().toString().c_str(), AP_SSID, AP_PASSWORD);
  Serial.printf("AP MAC: %s\n", WiFi.softAPmacAddress().c_str());

  Serial.println("HTTP receiver ready - waiting for belt data");
  Serial.printf("AP MAC: %s\n", WiFi.softAPmacAddress().c_str());

  // Captive portal DNS - redirect ALL domain lookups to our IP
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("DNS captive portal started");

  // Web server - serve inline dashboard
  server.on("/", HTTP_ANY, []() { sendDashboard(); });
  server.on("/index.html", HTTP_ANY, []() { sendDashboard(); });

  // API endpoint with CORS headers
  server.on("/api/data", HTTP_ANY, []() { sendApiData(); });

  // Android captive portal detection
  server.on("/generate_204", HTTP_GET, []() {
    server.send(204, "text/plain", "");
  });
  server.on("/gen_204", HTTP_GET, []() {
    server.send(204, "text/plain", "");
  });
  // iOS/macOS captive portal detection
  server.on("/hotspot-detect.html", HTTP_GET, []() {
    server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  // Windows captive portal detection
  server.on("/connecttest.txt", HTTP_GET, []() {
    server.send(200, "text/plain", "Microsoft Connect Test");
  });

  // Catch-all: serve dashboard for any GET, else 404
  server.onNotFound([]() {
    logHttpRequest();
    if (server.method() == HTTP_GET) {
      sendDashboard();
      return;
    }
    server.send(404, "text/plain", "Not Found");
  });

  server.begin();
  Serial.println("Web server started at http://192.168.4.1/");
  Serial.println("Waiting for Belt 2 data...\n");
}

// =====================================================
//  LOOP
// =====================================================
void loop() {
  // Turn off LED 100ms after it was turned on by callback
  static unsigned long ledOnTime = 0;
  if (digitalRead(STATUS_LED)) {
    if (ledOnTime == 0) ledOnTime = millis();
    if (millis() - ledOnTime > 100) {
      digitalWrite(STATUS_LED, LOW);
      ledOnTime = 0;
    }
  }

  // Process DNS requests (captive portal)
  dnsServer.processNextRequest();
  server.handleClient();

  // Heartbeat every 10 seconds so you know it's alive
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 10000) {
    Serial.printf("[Alive] Uptime: %lus | Packets: %d | Clients: %d | LastPacket: %lus ago\n",
      millis() / 1000, packetsReceived, WiFi.softAPgetStationNum(),
      lastPacketMs == 0 ? 0 : (millis() - lastPacketMs) / 1000);
    lastHeartbeat = millis();
  }
  delay(10);
}
