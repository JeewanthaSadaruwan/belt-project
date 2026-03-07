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
  --bg1:#0b1020;
  --bg2:#0f172a;
  --panel:#111827;
  --panel2:#0f172a;
  --line:#1f2937;
  --accent:#f97316;
  --good:#22c55e;
  --warn:#f59e0b;
  --bad:#ef4444;
  --text:#e5e7eb;
  --muted:#9ca3af;
}
body{
  font-family:"Palatino Linotype","Book Antiqua",Palatino,serif;
  background:
    radial-gradient(1200px 600px at 20% -10%, #1e293b 0%, #0b1020 60%),
    linear-gradient(180deg,var(--bg1) 0%, var(--bg2) 100%);
  color:var(--text);
  min-height:100vh;
}
header{
  padding:16px 22px;
  border-bottom:1px solid var(--line);
  display:flex;
  gap:12px;
  align-items:center;
  justify-content:space-between;
  background:linear-gradient(180deg,#0f172a 0%, #0b1020 100%);
}
.logo{
  font-size:1.3rem;
  letter-spacing:1px;
  text-transform:uppercase;
}
.badge{
  display:flex;
  align-items:center;
  gap:10px;
  padding:8px 14px;
  border:1px solid var(--line);
  border-radius:22px;
  background:#0b1020;
  font-size:0.85rem;
}
.dot{
  width:10px;height:10px;border-radius:50%;
  background:var(--bad);
}
.dot.on{background:var(--good);box-shadow:0 0 8px rgba(34,197,94,.6)}
.dot.warn{background:var(--warn);box-shadow:0 0 8px rgba(245,158,11,.6)}
.container{max-width:1200px;margin:0 auto;padding:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px}
.card{
  background:linear-gradient(180deg,var(--panel) 0%, var(--panel2) 100%);
  border:1px solid var(--line);
  border-radius:16px;
  overflow:hidden;
}
.card h2{
  padding:14px 18px;
  font-size:0.95rem;
  text-transform:uppercase;
  letter-spacing:1px;
  border-bottom:1px solid var(--line);
}
.kv{
  display:flex;
  justify-content:space-between;
  padding:10px 18px;
  border-bottom:1px dashed #1f2937;
  font-size:0.95rem;
}
.kv:last-child{border-bottom:none}
.label{color:var(--muted)}
.value{font-family:"Consolas","Lucida Console",monospace;font-weight:700}
.value.good{color:var(--good)}
.value.warn{color:var(--warn)}
.value.bad{color:var(--bad)}
.big{font-size:1.5rem}
.axis{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:10px;
  padding:12px 18px 18px;
}
.box{
  background:#0b1020;
  border:1px solid var(--line);
  border-radius:12px;
  text-align:center;
  padding:12px;
}
.axis-label{
  font-size:0.75rem;
  color:var(--muted);
  text-transform:uppercase;
  letter-spacing:1px;
}
.axis-val{
  font-size:1.2rem;
  margin-top:6px;
  font-family:"Consolas","Lucida Console",monospace;
}
.raw{
  background:#0b1020;
  border:1px solid var(--line);
  border-radius:12px;
  padding:12px;
  font-family:"Consolas","Lucida Console",monospace;
  font-size:0.8rem;
  color:#cbd5f5;
  max-height:200px;
  overflow:auto;
  white-space:pre-wrap;
}
.footer{
  text-align:center;
  padding:18px;
  color:#6b7280;
  font-size:0.8rem;
}
@media (max-width:700px){
  header{flex-direction:column;align-items:flex-start}
}
</style>
</head>
<body>
<header>
  <div class="logo">Belt Station 2</div>
  <div class="badge">
    <div class="dot" id="connDot"></div>
    <span id="connText">Waiting for packets...</span>
  </div>
</header>
<div class="container">
  <div class="grid">
    <section class="card">
      <h2>Link Status</h2>
      <div class="kv"><span class="label">Protocol</span><span class="value" id="protoText">SSE</span></div>
      <div class="kv"><span class="label">Packets</span><span class="value big" id="pkt">0</span></div>
      <div class="kv"><span class="label">Last Update</span><span class="value" id="upd">--</span></div>
      <div class="kv"><span class="label">Packet Age</span><span class="value" id="age">--</span></div>
    </section>
    <section class="card">
      <h2>GPS</h2>
      <div class="kv"><span class="label">Fix</span><span class="value" id="gpsStatus">--</span></div>
      <div class="kv"><span class="label">Latitude</span><span class="value" id="lat">--</span></div>
      <div class="kv"><span class="label">Longitude</span><span class="value" id="lng">--</span></div>
      <div class="kv"><span class="label">Altitude</span><span class="value" id="alt">--</span></div>
      <div class="kv"><span class="label">Speed</span><span class="value" id="spd">--</span></div>
      <div class="kv"><span class="label">Satellites</span><span class="value" id="sat">--</span></div>
    </section>
    <section class="card">
      <h2>Accelerometer (m/s2)</h2>
      <div class="axis">
        <div class="box"><div class="axis-label">X</div><div class="axis-val" id="ax">--</div></div>
        <div class="box"><div class="axis-label">Y</div><div class="axis-val" id="ay">--</div></div>
        <div class="box"><div class="axis-label">Z</div><div class="axis-val" id="az">--</div></div>
      </div>
    </section>
    <section class="card">
      <h2>Gyroscope (rad/s)</h2>
      <div class="axis">
        <div class="box"><div class="axis-label">X</div><div class="axis-val" id="gx">--</div></div>
        <div class="box"><div class="axis-label">Y</div><div class="axis-val" id="gy">--</div></div>
        <div class="box"><div class="axis-label">Z</div><div class="axis-val" id="gz">--</div></div>
      </div>
    </section>
    <section class="card">
      <h2>System</h2>
      <div class="kv"><span class="label">IMU</span><span class="value" id="imuSt">--</span></div>
      <div class="kv"><span class="label">Temperature</span><span class="value" id="temp">--</span></div>
    </section>
    <section class="card">
      <h2>Raw JSON</h2>
      <div class="raw" id="raw">{}</div>
    </section>
  </div>
</div>
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
