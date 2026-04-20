/*
 * BASE STATION 2 — WiFi HTTP Receiver + Tactical HUD Dashboard
 * Receives sensor data from Belt 2 via HTTP POST
 * Serves full tactical dashboard at http://192.168.4.1/
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

// Base station target coordinates (updated via /api/setbase from dashboard)
double storedBaseLat = 6.934200;
double storedBaseLng = 79.850600;
bool   baseLocationConfirmed = false;
bool   navEnabled = false;  // true once user presses START NAVIGATION on dashboard

// =====================================================================
//  TACTICAL HUD DASHBOARD  (no external dependencies — local AP only)
// =====================================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BELT NAV — OPS CONSOLE</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:       #040810;
  --bg2:      #060c16;
  --panel:    #080e1c;
  --panel2:   #060b18;
  --border:   #0d1f3a;
  --green:    #00e676;
  --green2:   #00ff9d;
  --amber:    #ffab00;
  --red:      #ff3d57;
  --blue:     #29b6f6;
  --purple:   #b388ff;
  --dim:      #2a3a5a;
  --text:     #c8d8f0;
  --muted:    #4a607a;
  --font:     'Courier New', 'Lucida Console', monospace;
}

/* ── Scanline + noise overlay ── */
body::before{
  content:'';
  position:fixed;
  inset:0;
  pointer-events:none;
  z-index:9999;
  background:repeating-linear-gradient(
    0deg,
    transparent,
    transparent 2px,
    rgba(0,230,118,.018) 2px,
    rgba(0,230,118,.018) 4px
  );
}
body{
  font-family:var(--font);
  background:var(--bg);
  color:var(--text);
  min-height:100vh;
  overflow-x:hidden;
}

/* ── Topbar ── */
.topbar{
  position:sticky;top:0;z-index:100;
  display:flex;align-items:center;justify-content:space-between;
  padding:10px 20px;
  background:rgba(4,8,16,.96);
  border-bottom:1px solid var(--border);
  box-shadow:0 0 30px rgba(0,230,118,.06);
}
.brand{display:flex;flex-direction:column;gap:2px}
.brand-title{
  font-size:1.1rem;letter-spacing:4px;
  color:var(--green);text-transform:uppercase;
  text-shadow:0 0 18px rgba(0,230,118,.6);
}
.brand-sub{font-size:.7rem;letter-spacing:3px;color:var(--muted)}
.topbar-right{display:flex;gap:14px;align-items:center}
.pill{
  display:flex;align-items:center;gap:8px;
  padding:5px 12px;border:1px solid var(--border);
  border-radius:4px;background:rgba(8,14,28,.9);
  font-size:.72rem;letter-spacing:1.5px;
}
.dot{width:8px;height:8px;border-radius:50%;background:var(--red)}
.dot.live{background:var(--green);box-shadow:0 0 10px rgba(0,230,118,.8);animation:blink 1.4s infinite}
.dot.warn{background:var(--amber);box-shadow:0 0 10px rgba(255,171,0,.7)}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.4}}

/* ── Grid layout ── */
.wrap{max-width:1400px;margin:0 auto;padding:14px;display:flex;flex-direction:column;gap:14px}

.row{display:grid;gap:14px}
.row-top{grid-template-columns:1fr 280px 1fr}
.row-mid{grid-template-columns:1.5fr 1fr;grid-template-rows:auto auto;grid-template-areas:"gps hr" "tilt gyro"}
.row-bot{grid-template-columns:1fr 1fr}

/* ── Panel base ── */
.panel{
  background:linear-gradient(160deg,var(--panel) 0%,var(--panel2) 100%);
  border:1px solid var(--border);
  border-radius:6px;
  padding:14px;
  position:relative;
  overflow:hidden;
}
/* corner notches */
.panel::before,.panel::after{
  content:'';position:absolute;width:10px;height:10px;
  border-color:var(--green);border-style:solid;opacity:.5;
}
.panel::before{top:0;left:0;border-width:1px 0 0 1px}
.panel::after{bottom:0;right:0;border-width:0 1px 1px 0}

.ph{
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:12px;padding-bottom:8px;
  border-bottom:1px solid var(--border);
}
.ph-label{font-size:.65rem;letter-spacing:2.5px;color:var(--muted);text-transform:uppercase}
.ph-title{font-size:.85rem;letter-spacing:2px;color:var(--text);text-transform:uppercase;margin-top:2px}
.tag{
  font-size:.65rem;letter-spacing:1.5px;padding:3px 8px;
  border:1px solid var(--border);border-radius:3px;
  color:var(--muted);background:rgba(0,0,0,.4);
}
.tag.green{border-color:rgba(0,230,118,.4);color:var(--green)}
.tag.amber{border-color:rgba(255,171,0,.4);color:var(--amber)}
.tag.red{border-color:rgba(255,61,87,.4);color:var(--red)}
.tag.blue{border-color:rgba(41,182,246,.4);color:var(--blue)}

/* ── NAV STATE big display ── */
.nav-state-panel{
  display:flex;flex-direction:column;align-items:center;
  justify-content:center;padding:20px;gap:8px;
}
.nav-state-text{
  font-size:1.5rem;letter-spacing:4px;text-transform:uppercase;
  text-align:center;transition:all .3s;
}
.nav-state-text.arrived{color:var(--green);text-shadow:0 0 30px rgba(0,230,118,.8)}
.nav-state-text.forward{color:var(--green2);text-shadow:0 0 20px rgba(0,255,157,.5)}
.nav-state-text.turn{color:var(--amber);text-shadow:0 0 20px rgba(255,171,0,.5)}
.nav-state-text.back{color:var(--red);text-shadow:0 0 20px rgba(255,61,87,.5)}
.nav-state-text.nogps{color:var(--muted)}

/* ── Direction arrow ── */
.dir-arrow-wrap{
  display:flex;flex-direction:column;align-items:center;gap:6px;
}
#dirArrowSvg{transition:transform .4s ease;filter:drop-shadow(0 0 12px rgba(0,230,118,.5))}
.dir-label{font-size:.7rem;letter-spacing:2px;color:var(--muted)}

/* ── Compass ── */
.compass-wrap{
  display:flex;flex-direction:column;align-items:center;gap:8px;
}
#compassCanvas{border-radius:50%}
.compass-vals{
  display:flex;gap:20px;justify-content:center;
  font-size:.72rem;letter-spacing:1.5px;color:var(--muted);
}
.compass-vals span{display:flex;flex-direction:column;align-items:center;gap:3px}
.compass-vals .cv{color:var(--text);font-size:.85rem}

/* ── Distance meter ── */
.dist-panel{display:flex;flex-direction:column;gap:10px}
.dist-num{
  font-size:2.6rem;letter-spacing:2px;color:var(--green);
  text-shadow:0 0 20px rgba(0,230,118,.4);
  text-align:center;
}
.dist-unit{font-size:.75rem;letter-spacing:2px;color:var(--muted);text-align:center}
.meter-bar{
  height:6px;background:rgba(255,255,255,.06);
  border-radius:3px;overflow:hidden;
  border:1px solid var(--border);
}
.meter-fill{
  height:100%;
  background:linear-gradient(90deg,var(--green),var(--green2));
  border-radius:3px;
  transition:width .6s ease;
  box-shadow:0 0 10px rgba(0,230,118,.4);
}
.dist-rows{display:flex;flex-direction:column;gap:4px;margin-top:4px}
.drow{display:flex;justify-content:space-between;font-size:.75rem;padding:4px 0;border-bottom:1px solid rgba(13,31,58,.8)}
.drow:last-child{border:none}
.drow .dk{color:var(--muted);letter-spacing:1px}
.drow .dv{color:var(--text);font-size:.8rem}
.drow .dv.green{color:var(--green)}
.drow .dv.amber{color:var(--amber)}
.drow .dv.red{color:var(--red)}
.drow .dv.blue{color:var(--blue)}

/* ── GPS ── */
.gps-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.gps-stat{
  background:rgba(0,0,0,.3);border:1px solid var(--border);
  border-radius:4px;padding:9px;
}
.gs-label{font-size:.62rem;letter-spacing:1.5px;color:var(--muted);text-transform:uppercase}
.gs-val{font-size:.9rem;margin-top:4px;color:var(--text)}
.gs-val.green{color:var(--green)}
.gs-val.amber{color:var(--amber)}
.gs-val.red{color:var(--red)}
.sat-bars{display:flex;align-items:flex-end;gap:3px;height:22px;margin-top:4px}
.sat-bar{width:10px;background:var(--dim);border-radius:2px;transition:all .4s}
.sat-bar.lit{background:var(--green);box-shadow:0 0 6px rgba(0,230,118,.5)}

/* ── IMU tilt ── */
.tilt-wrap{display:flex;flex-direction:column;align-items:center;gap:10px}
#tiltCanvas{border:1px solid var(--border);border-radius:50%}
.axis-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;width:100%}
.ax-box{
  background:rgba(0,0,0,.3);border:1px solid var(--border);
  border-radius:4px;text-align:center;padding:7px 4px;
}
.ax-lbl{font-size:.6rem;letter-spacing:1.5px;color:var(--muted)}
.ax-val{font-size:.85rem;margin-top:3px}

/* ── Gyro ── */
.gyro-bars{display:flex;flex-direction:column;gap:7px;margin-top:4px}
.gbar-row{display:flex;align-items:center;gap:8px}
.gbar-label{font-size:.68rem;letter-spacing:1.5px;color:var(--muted);width:16px}
.gbar-track{
  flex:1;height:8px;background:rgba(255,255,255,.04);
  border:1px solid var(--border);border-radius:4px;
  position:relative;overflow:visible;
}
.gbar-fill{
  position:absolute;top:0;height:100%;border-radius:4px;
  transition:all .15s ease;
}
.gbar-center{position:absolute;top:-2px;left:50%;width:1px;height:12px;background:var(--dim)}
.gbar-val{font-size:.7rem;color:var(--text);width:46px;text-align:right}

/* ── Heart rate ── */
.hr-wrap{display:flex;flex-direction:column;align-items:center;gap:8px}
.hr-num{
  font-size:3rem;letter-spacing:2px;
  text-align:center;transition:color .3s;
}
.hr-num.active{color:var(--red);text-shadow:0 0 25px rgba(255,61,87,.5)}
.hr-num.inactive{color:var(--muted)}
.hr-unit{font-size:.7rem;letter-spacing:2px;color:var(--muted);text-align:center}
.hr-status{font-size:.72rem;letter-spacing:1.5px;text-align:center;margin-top:2px}
.hr-status.on{color:var(--red)}
.hr-status.off{color:var(--muted)}
#hrCanvas{border:1px solid var(--border);border-radius:4px}

/* ── Heading source badge ── */
.hsrc{
  display:inline-flex;align-items:center;gap:5px;
  font-size:.68rem;letter-spacing:1.5px;padding:3px 8px;
  border:1px solid;border-radius:3px;
}
.hsrc.GPS{border-color:rgba(41,182,246,.5);color:var(--blue)}
.hsrc.GYRO{border-color:rgba(179,136,255,.5);color:var(--purple)}
.hsrc.STALE{border-color:rgba(255,61,87,.4);color:var(--red);animation:blink 1s infinite}
.hsrc.NONE{border-color:var(--border);color:var(--muted)}

/* ── Raw JSON ── */
.raw-box{
  background:rgba(0,0,0,.5);border:1px solid var(--border);
  border-radius:4px;padding:10px;
  font-size:.72rem;color:#5a7a9a;
  max-height:160px;overflow:auto;
  white-space:pre-wrap;word-break:break-all;
  line-height:1.5;
}

/* ── Telemetry ticker ── */
.ticker-row{
  display:flex;gap:16px;overflow:hidden;
  font-size:.68rem;letter-spacing:1.5px;color:var(--muted);
  border-top:1px solid var(--border);padding-top:10px;
  flex-wrap:wrap;
}
.ticker-item{display:flex;gap:6px}
.tk{color:var(--muted)}
.tv{color:var(--text)}

/* ── Packet stream ── */
.pkt-count{
  font-size:2.4rem;color:var(--blue);
  text-shadow:0 0 15px rgba(41,182,246,.4);
  text-align:center;
}
.pkt-label{font-size:.65rem;letter-spacing:2px;color:var(--muted);text-align:center}

@media(max-width:1100px){
  .row-top{grid-template-columns:1fr 1fr}
  .row-mid{grid-template-columns:1fr 1fr}
  .row-bot{grid-template-columns:1fr}
}
@media(max-width:680px){
  .row-top,.row-bot{grid-template-columns:1fr}
  .row-mid{grid-template-columns:1fr;grid-template-areas:"gps" "hr" "tilt" "gyro"}
  .topbar{flex-direction:column;gap:8px;align-items:flex-start}
}
</style>
</head>
<body>

<!-- TOPBAR -->
<header class="topbar">
  <div class="brand">
    <div class="brand-title">&#9632; BELT NAV — OPS CONSOLE</div>
    <div class="brand-sub">REAL-TIME FIELD TELEMETRY &nbsp;|&nbsp; HTTP RECEIVER &nbsp;|&nbsp; 192.168.4.1</div>
  </div>
  <div class="topbar-right">
    <div class="pill">
      <div class="dot" id="connDot"></div>
      <span id="connText" style="letter-spacing:1.5px">WAITING...</span>
    </div>
    <div class="pill">PKT <span id="pktTop" style="color:#29b6f6;margin-left:4px">0</span></div>
    <div class="pill">AGE <span id="ageTop" style="color:#ffab00;margin-left:4px">--</span></div>
  </div>
</header>

<main class="wrap">

  <!-- ROW 1: Direction · Compass · Distance -->
  <div class="row row-top">

    <!-- Nav state + direction arrow -->
    <div class="panel">
      <div class="ph">
        <div><div class="ph-label">Navigation</div><div class="ph-title">Direction Cue</div></div>
        <span class="tag" id="hdgSrcTag">SRC: --</span>
      </div>

      <!-- STANDBY: shown until user presses START -->
      <div id="navStandby" style="display:flex;flex-direction:column;align-items:center;justify-content:center;padding:24px 16px;gap:14px">
        <div style="font-size:.72rem;letter-spacing:3px;color:var(--muted);text-transform:uppercase;text-align:center">USER NOT STARTED</div>
        <div style="font-size:.65rem;letter-spacing:1.5px;color:var(--dim);text-align:center">Sensor data streaming — press START to begin navigation</div>
        <button onclick="startNav()" id="startNavBtn"
          style="background:rgba(255,61,87,.12);border:2px solid rgba(255,61,87,.7);color:#ff3d57;padding:14px 0;font-family:var(--font);font-size:.85rem;letter-spacing:3px;border-radius:5px;cursor:pointer;width:100%;transition:all .2s;text-shadow:0 0 12px rgba(255,61,87,.5)"
          onmouseover="this.style.background='rgba(255,61,87,.28)'" onmouseout="this.style.background='rgba(255,61,87,.12)'">
          &#9654; START NAVIGATION
        </button>
      </div>

      <!-- ACTIVE: shown when navigation is running -->
      <div class="nav-state-panel" id="navActive" style="display:none">
        <div class="dir-arrow-wrap">
          <svg id="dirArrowSvg" width="90" height="90" viewBox="-45 -45 90 90">
            <defs>
              <filter id="glow">
                <feGaussianBlur stdDeviation="3" result="blur"/>
                <feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge>
              </filter>
            </defs>
            <polygon id="dirArrow"
              points="0,-36 12,-12 4,-12 4,36 -4,36 -4,-12 -12,-12"
              fill="#00e676" filter="url(#glow)" opacity="0.9"/>
            <circle r="5" fill="none" stroke="#00e676" stroke-width="1.5" opacity="0.5"/>
          </svg>
          <div class="dir-label" id="relBearLabel">REL BEARING: --</div>
        </div>
        <div class="nav-state-text nogps" id="navStateText">AWAITING GPS</div>
        <div style="font-size:.7rem;letter-spacing:2px;color:var(--muted);text-align:center" id="navSubtext">--</div>
        <button onclick="stopNav()"
          style="margin-top:10px;background:rgba(255,171,0,.08);border:1px solid rgba(255,171,0,.4);color:#ffab00;padding:7px 22px;font-family:var(--font);font-size:.68rem;letter-spacing:2px;border-radius:4px;cursor:pointer;transition:background .2s"
          onmouseover="this.style.background='rgba(255,171,0,.22)'" onmouseout="this.style.background='rgba(255,171,0,.08)'">
          &#9632; STOP NAV
        </button>
      </div>
    </div>

    <!-- Compass rose canvas -->
    <div class="panel">
      <div class="ph">
        <div><div class="ph-label">Orientation</div><div class="ph-title">Compass</div></div>
        <span class="tag blue">FUSED HDG</span>
      </div>
      <div class="compass-wrap">
        <canvas id="compassCanvas" width="200" height="200"></canvas>
        <div class="compass-vals">
          <span>HEADING<div class="cv" id="cvHeading">---°</div></span>
          <span>TO BASE<div class="cv" id="cvBearing">---°</div></span>
          <span>REL<div class="cv" id="cvRel">---°</div></span>
        </div>
      </div>
    </div>

    <!-- Distance + telemetry -->
    <div class="panel">
      <div class="ph">
        <div><div class="ph-label">Target</div><div class="ph-title">Distance</div></div>
        <span class="tag" id="arrivedTag">EN ROUTE</span>
      </div>
      <div class="dist-panel">
        <div class="dist-num" id="distNum">---</div>
        <div class="dist-unit">METRES TO BASE</div>
        <div class="meter-bar"><div class="meter-fill" id="distBar" style="width:0%"></div></div>
        <div class="dist-rows">
          <div class="drow"><span class="dk">STATE</span><span class="dv" id="dr-state">--</span></div>
          <div class="drow"><span class="dk">BEARING</span><span class="dv blue" id="dr-bearing">--</span></div>
          <div class="drow"><span class="dk">SPEED</span><span class="dv" id="dr-speed">--</span></div>
          <div class="drow"><span class="dk">ALTITUDE</span><span class="dv" id="dr-alt">--</span></div>
          <div class="drow"><span class="dk">PACKETS RX</span><span class="dv blue" id="dr-pkts">0</span></div>
          <div class="drow"><span class="dk">LAST PKT</span><span class="dv" id="dr-age">--</span></div>
        </div>
      </div>
    </div>
  </div>

  <!-- ROW 2: GPS · Tilt · Gyro · Heart -->
  <div class="row row-mid">

    <!-- GPS -->
    <div class="panel" style="grid-area:gps">
      <div class="ph"><div class="ph-title">GPS Fix</div></div>
        <span class="tag" id="gpsFixTag">NO FIX</span>
      </div>
      <div class="gps-grid">
        <div class="gps-stat">
          <div class="gs-label">Latitude</div>
          <div class="gs-val" id="g-lat">--</div>
        </div>
        <div class="gps-stat">
          <div class="gs-label">Longitude</div>
          <div class="gs-val" id="g-lng">--</div>
        </div>
        <div class="gps-stat">
          <div class="gs-label">Altitude</div>
          <div class="gs-val" id="g-alt">--</div>
        </div>
        <div class="gps-stat">
          <div class="gs-label">Speed</div>
          <div class="gs-val" id="g-spd">--</div>
        </div>
      </div>
      <div class="gps-stat" style="margin-top:8px">
        <div class="gs-label">Satellites</div>
        <div style="display:flex;align-items:center;justify-content:space-between">
          <div class="gs-val" id="g-sats">--</div>
          <div class="sat-bars" id="satBars">
            <!-- filled by JS -->
          </div>
        </div>
      </div>
    </div>

    <!-- IMU tilt bubble -->
    <div class="panel" style="grid-area:tilt">
      <div class="ph">
        <div><div class="ph-label">Accelerometer</div><div class="ph-title">Tilt / Accel</div></div>
        <span class="tag" id="imuTag">IMU</span>
      </div>
      <div class="tilt-wrap">
        <canvas id="tiltCanvas" width="140" height="140"></canvas>
        <div class="axis-grid">
          <div class="ax-box">
            <div class="ax-lbl">AX</div>
            <div class="ax-val" id="a-x" style="color:#29b6f6">--</div>
          </div>
          <div class="ax-box">
            <div class="ax-lbl">AY</div>
            <div class="ax-val" id="a-y" style="color:#b388ff">--</div>
          </div>
          <div class="ax-box">
            <div class="ax-lbl">AZ</div>
            <div class="ax-val" id="a-z" style="color:#00e676">--</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Gyro bars -->
    <div class="panel" style="grid-area:gyro">
      <div class="ph">
        <div><div class="ph-label">Gyroscope</div><div class="ph-title">Angular Rate</div></div>
        <span class="tag">rad/s</span>
      </div>
      <div style="margin-top:4px">
        <div style="font-size:.62rem;letter-spacing:1.5px;color:var(--muted);margin-bottom:10px">
          ROTATION RATE — DEAD ZONE ±0.01
        </div>
        <div class="gyro-bars">
          <div class="gbar-row">
            <span class="gbar-label">X</span>
            <div class="gbar-track">
              <div class="gbar-center"></div>
              <div class="gbar-fill" id="gbx" style="background:#29b6f6"></div>
            </div>
            <span class="gbar-val" id="gv-x" style="color:#29b6f6">--</span>
          </div>
          <div class="gbar-row">
            <span class="gbar-label">Y</span>
            <div class="gbar-track">
              <div class="gbar-center"></div>
              <div class="gbar-fill" id="gby" style="background:#b388ff"></div>
            </div>
            <span class="gbar-val" id="gv-y" style="color:#b388ff">--</span>
          </div>
          <div class="gbar-row">
            <span class="gbar-label">Z</span>
            <div class="gbar-track">
              <div class="gbar-center"></div>
              <div class="gbar-fill" id="gbz" style="background:#00e676"></div>
            </div>
            <span class="gbar-val" id="gv-z" style="color:#00e676">--</span>
          </div>
        </div>
        <div style="margin-top:14px;border-top:1px solid var(--border);padding-top:10px">
          <div class="dist-rows">
            <div class="drow"><span class="dk">TEMP</span><span class="dv amber" id="imu-temp">--</span></div>
            <div class="drow"><span class="dk">BIAS Z</span><span class="dv" id="imu-bias">--</span></div>
          </div>
        </div>
      </div>
    </div>

    <!-- Heart rate -->
    <div class="panel" style="grid-area:hr">
      <div class="ph">
        <div><div class="ph-label">MAX30102</div><div class="ph-title">Heart Rate</div></div>
        <span class="tag" id="hrTag">HR SENSOR</span>
      </div>
      <div class="hr-wrap">
        <div class="hr-num inactive" id="hrNum">---</div>
        <div class="hr-unit">BPM</div>
        <div class="hr-status off" id="hrStatus">&#9632; NO FINGER</div>
        <canvas id="hrCanvas" width="200" height="60"></canvas>
      </div>
    </div>

  </div>

  <!-- ROW 3: Ticker + Raw -->
  <div class="row row-bot">

    <!-- Telemetry summary ticker -->
    <div class="panel">
      <div class="ph">
        <div><div class="ph-label">Telemetry</div><div class="ph-title">Live Feed</div></div>
        <div class="pkt-count" id="pktBig">0</div>
      </div>
      <div class="ticker-row" id="ticker">
        <div class="ticker-item"><span class="tk">TIME</span><span class="tv" id="tk-time">--</span></div>
        <div class="ticker-item"><span class="tk">GPS</span><span class="tv" id="tk-gps">--</span></div>
        <div class="ticker-item"><span class="tk">HDG</span><span class="tv" id="tk-hdg">--°</span></div>
        <div class="ticker-item"><span class="tk">DIST</span><span class="tv" id="tk-dist">-- m</span></div>
        <div class="ticker-item"><span class="tk">STATE</span><span class="tv" id="tk-state">--</span></div>
        <div class="ticker-item"><span class="tk">HR</span><span class="tv" id="tk-hr">--</span></div>
        <div class="ticker-item"><span class="tk">IMU</span><span class="tv" id="tk-imu">--</span></div>
        <div class="ticker-item"><span class="tk">UPTIME</span><span class="tv" id="tk-uptime">--</span></div>
      </div>
    </div>

    <!-- Raw JSON -->
    <div class="panel">
      <div class="ph">
        <div><div class="ph-label">Debug</div><div class="ph-title">JSON Stream</div></div>
        <span class="tag">/api/data</span>
      </div>
      <div class="raw-box" id="rawJson">{}</div>
    </div>
  </div>

  <!-- BASE STATION LOCATION SETTER -->
  <div class="panel" id="baseSetPanel" style="order:-1;border-color:rgba(255,61,87,.4)">
    <div class="ph">
      <div><div class="ph-label">Navigation Prerequisite</div><div class="ph-title">Base Station Location</div></div>
      <span class="tag red" id="baseSetTag">SET REQUIRED</span>
    </div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px">
      <div class="gps-stat">
        <div class="gs-label">Current Latitude</div>
        <div class="gs-val green" id="base-cur-lat">6.934200</div>
      </div>
      <div class="gps-stat">
        <div class="gs-label">Current Longitude</div>
        <div class="gs-val green" id="base-cur-lng">79.850600</div>
      </div>
    </div>
    <div style="display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:flex-end">
      <div>
        <div class="gs-label" style="margin-bottom:5px">New Latitude</div>
        <input id="inp-lat" type="number" step="0.000001" placeholder="e.g. 6.927079"
          style="width:100%;background:#040810;border:1px solid #0d1f3a;color:#c8d8f0;padding:8px 10px;font-family:var(--font);font-size:.8rem;border-radius:4px;letter-spacing:1px;outline:none">
      </div>
      <div>
        <div class="gs-label" style="margin-bottom:5px">New Longitude</div>
        <input id="inp-lng" type="number" step="0.000001" placeholder="e.g. 79.861244"
          style="width:100%;background:#040810;border:1px solid #0d1f3a;color:#c8d8f0;padding:8px 10px;font-family:var(--font);font-size:.8rem;border-radius:4px;letter-spacing:1px;outline:none">
      </div>
      <button onclick="setBase()"
        style="background:rgba(0,230,118,.1);border:1px solid rgba(0,230,118,.4);color:#00e676;padding:8px 18px;font-family:var(--font);font-size:.72rem;letter-spacing:2px;border-radius:4px;cursor:pointer;white-space:nowrap;transition:background .2s"
        onmouseover="this.style.background='rgba(0,230,118,.2)'" onmouseout="this.style.background='rgba(0,230,118,.1)'">
        &#9654; SET BASE
      </button>
    </div>
    <div id="base-msg" style="font-size:.72rem;letter-spacing:1.5px;color:var(--muted);text-align:right;margin-top:8px"></div>
  </div>

</main>

<script>
// ── Globals ──────────────────────────────────────────────────
var lastPacketAt = 0;
var startedAt    = Date.now();
var hrHistory    = new Array(100).fill(0);
var lastHrBPM    = 0;

// ── Compass canvas ───────────────────────────────────────────
var compassCv  = document.getElementById('compassCanvas');
var compassCtx = compassCv.getContext('2d');

function drawCompass(heading, bearing) {
  var cx = 100, cy = 100, r = 90;
  var ctx = compassCtx;
  ctx.clearRect(0,0,200,200);

  // Background
  ctx.beginPath();
  ctx.arc(cx,cy,r,0,Math.PI*2);
  ctx.fillStyle = '#060b18';
  ctx.fill();
  ctx.strokeStyle = '#0d1f3a';
  ctx.lineWidth = 1.5;
  ctx.stroke();

  // Tick marks
  for(var i=0;i<360;i+=10){
    var a = (i-90)*Math.PI/180;
    var isMajor = i%90===0, isMid = i%30===0;
    var len = isMajor ? 14 : isMid ? 9 : 5;
    var innerR = r - len;
    ctx.beginPath();
    ctx.moveTo(cx + Math.cos(a)*innerR, cy + Math.sin(a)*innerR);
    ctx.lineTo(cx + Math.cos(a)*(r-2),  cy + Math.sin(a)*(r-2));
    ctx.strokeStyle = isMajor ? '#00e676' : isMid ? '#1e4040' : '#0d2020';
    ctx.lineWidth = isMajor ? 1.5 : 1;
    ctx.stroke();
  }

  // Cardinal labels
  var cardinals = [{l:'N',a:0},{l:'E',a:90},{l:'S',a:180},{l:'W',a:270}];
  ctx.font = 'bold 11px Courier New';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  cardinals.forEach(function(c){
    var a = (c.a-90)*Math.PI/180;
    var lx = cx + Math.cos(a)*(r-22);
    var ly = cy + Math.sin(a)*(r-22);
    ctx.fillStyle = c.l==='N' ? '#00e676' : '#2a5a5a';
    ctx.fillText(c.l, lx, ly);
  });

  // Bearing line to base (green dashed)
  if(bearing !== null){
    var ba = (bearing-90)*Math.PI/180;
    ctx.beginPath();
    ctx.setLineDash([5,4]);
    ctx.moveTo(cx,cy);
    ctx.lineTo(cx + Math.cos(ba)*(r-6), cy + Math.sin(ba)*(r-6));
    ctx.strokeStyle = 'rgba(0,230,118,.5)';
    ctx.lineWidth = 1.5;
    ctx.stroke();
    ctx.setLineDash([]);
    // Target dot
    ctx.beginPath();
    ctx.arc(cx + Math.cos(ba)*(r-10), cy + Math.sin(ba)*(r-10), 4, 0, Math.PI*2);
    ctx.fillStyle = '#00e676';
    ctx.fill();
  }

  // Heading needle (amber)
  if(heading !== null){
    var ha = (heading-90)*Math.PI/180;
    // Needle body
    ctx.beginPath();
    ctx.moveTo(cx + Math.cos(ha)*(r-8), cy + Math.sin(ha)*(r-8));
    ctx.lineTo(cx, cy);
    ctx.strokeStyle = '#ffab00';
    ctx.lineWidth = 2;
    ctx.stroke();
    // Arrow head
    ctx.beginPath();
    ctx.arc(cx + Math.cos(ha)*(r-10), cy + Math.sin(ha)*(r-10), 5, 0, Math.PI*2);
    ctx.fillStyle = '#ffab00';
    ctx.fill();
    // Opposite tail
    ctx.beginPath();
    var ha2 = ha + Math.PI;
    ctx.moveTo(cx, cy);
    ctx.lineTo(cx + Math.cos(ha2)*18, cy + Math.sin(ha2)*18);
    ctx.strokeStyle = 'rgba(255,171,0,.3)';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  // Center dot
  ctx.beginPath();
  ctx.arc(cx,cy,5,0,Math.PI*2);
  ctx.fillStyle = '#040810';
  ctx.fill();
  ctx.beginPath();
  ctx.arc(cx,cy,3,0,Math.PI*2);
  ctx.fillStyle = '#ffab00';
  ctx.fill();
}

// ── Tilt bubble canvas ───────────────────────────────────────
var tiltCv  = document.getElementById('tiltCanvas');
var tiltCtx = tiltCv.getContext('2d');

function drawTilt(ax, ay) {
  var cx = 70, cy = 70, r = 60;
  var ctx = tiltCtx;
  ctx.clearRect(0,0,140,140);

  // Background
  ctx.beginPath();
  ctx.arc(cx,cy,r,0,Math.PI*2);
  ctx.fillStyle = '#040810';
  ctx.fill();
  ctx.strokeStyle = '#0d1f3a';
  ctx.lineWidth = 1.5;
  ctx.stroke();

  // Grid rings
  [0.3,0.6,1.0].forEach(function(f){
    ctx.beginPath();
    ctx.arc(cx,cy,r*f,0,Math.PI*2);
    ctx.strokeStyle = '#0d1f3a';
    ctx.lineWidth = 1;
    ctx.stroke();
  });
  // Crosshair
  ctx.beginPath();
  ctx.moveTo(cx-r,cy); ctx.lineTo(cx+r,cy);
  ctx.moveTo(cx,cy-r); ctx.lineTo(cx,cy+r);
  ctx.strokeStyle = '#0d1f3a';
  ctx.lineWidth = 1;
  ctx.stroke();

  // Bubble position (clamp to circle)
  var g = 9.81;
  var bx = cx + (ay/g)*r*0.8;
  var by = cy + (ax/g)*r*0.8;
  var dx = bx-cx, dy = by-cy;
  var dist = Math.sqrt(dx*dx+dy*dy);
  if(dist > r*0.85){ bx = cx + dx/dist*r*0.85; by = cy + dy/dist*r*0.85; }

  // Bubble glow
  var grd = ctx.createRadialGradient(bx,by,0,bx,by,16);
  grd.addColorStop(0,'rgba(0,230,118,.5)');
  grd.addColorStop(1,'rgba(0,230,118,0)');
  ctx.beginPath();
  ctx.arc(bx,by,16,0,Math.PI*2);
  ctx.fillStyle = grd;
  ctx.fill();

  // Bubble
  ctx.beginPath();
  ctx.arc(bx,by,8,0,Math.PI*2);
  ctx.fillStyle = '#00e676';
  ctx.fill();
  ctx.strokeStyle = '#00ff9d';
  ctx.lineWidth = 1.5;
  ctx.stroke();
}

// ── Heart rate canvas ────────────────────────────────────────
var hrCv  = document.getElementById('hrCanvas');
var hrCtx = hrCv.getContext('2d');

function drawHR(bpm, finger) {
  var w = 200, h = 60;
  var ctx = hrCtx;
  ctx.clearRect(0,0,w,h);
  ctx.fillStyle = 'rgba(0,0,0,.3)';
  ctx.fillRect(0,0,w,h);

  if(!finger || hrHistory.every(function(v){return v===0})){
    ctx.strokeStyle = '#1e3a5a';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0,h/2); ctx.lineTo(w,h/2);
    ctx.stroke();
    ctx.fillStyle = '#2a4a6a';
    ctx.font = '10px Courier New';
    ctx.textAlign = 'center';
    ctx.fillText('NO SIGNAL', w/2, h/2+4);
    return;
  }

  // Plot heartrate waveform from history
  var step = w / hrHistory.length;
  var max = Math.max.apply(null, hrHistory) || 1;
  var min = Math.min.apply(null, hrHistory);
  var range = max - min || 1;

  ctx.strokeStyle = '#ff3d57';
  ctx.lineWidth = 1.5;
  ctx.shadowColor = 'rgba(255,61,87,.5)';
  ctx.shadowBlur = 6;
  ctx.beginPath();
  hrHistory.forEach(function(v,i){
    var x = i*step;
    var y = h - ((v-min)/range)*(h-10)-5;
    if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.stroke();
  ctx.shadowBlur = 0;
}

// ── Direction arrow ──────────────────────────────────────────
function updateArrow(relBearing, state) {
  var arrow = document.getElementById('dirArrowSvg');
  var poly  = document.getElementById('dirArrow');
  var state_l = (state||'').toLowerCase();

  var color = '#4a607a'; // dim default
  if(state_l.indexOf('forward')>=0) color = '#00ff9d';
  else if(state_l.indexOf('right')>=0||state_l.indexOf('left')>=0) color = '#ffab00';
  else if(state_l.indexOf('back')>=0) color = '#ff3d57';
  else if(state_l.indexOf('arrived')>=0) color = '#00e676';

  poly.setAttribute('fill', color);
  poly.style.filter = 'drop-shadow(0 0 8px ' + color + ')';

  if(relBearing !== null){
    arrow.style.transform = 'rotate(' + relBearing + 'deg)';
  }
}

// ── Nav state text ───────────────────────────────────────────
function updateNavState(state, relBearing, dist) {
  var el  = document.getElementById('navStateText');
  var sub = document.getElementById('navSubtext');
  var tag = document.getElementById('arrivedTag');

  var cls = 'nogps', txt = state || 'AWAITING GPS', subTxt = '';

  if(!state){ txt = 'AWAITING GPS'; cls='nogps'; }
  else if(state.indexOf('FORWARD')>=0){ cls='forward'; txt='&#9650; GO FORWARD'; subTxt='STAY ON COURSE'; }
  else if(state.indexOf('RIGHT')>=0)  { cls='turn';    txt='&#9654; TURN RIGHT';  subTxt='ROTATE RIGHT'; }
  else if(state.indexOf('LEFT')>=0)   { cls='turn';    txt='&#9664; TURN LEFT';   subTxt='ROTATE LEFT'; }
  else if(state.indexOf('BACK')>=0)   { cls='back';    txt='&#9660; TURN AROUND'; subTxt='TARGET BEHIND YOU'; }
  else if(state.indexOf('ARRIVED')>=0){ cls='arrived'; txt='&#10003; ARRIVED';    subTxt='TARGET REACHED'; tag.textContent='ARRIVED'; tag.className='tag green'; }
  else if(state.indexOf('NO GPS')>=0) { cls='nogps';   txt='NO GPS FIX'; subTxt='WAITING FOR LOCK'; }
  else if(state.indexOf('WALK')>=0)   { cls='nogps';   txt='START WALKING'; subTxt='NEED HEADING LOCK'; }
  else if(state.indexOf('SET BASE')>=0){ cls='nogps';   txt='&#128273; BASE REQUIRED'; subTxt='SET BASE LOCATION ABOVE'; }
  else if(state.indexOf('STANDBY')>=0){ cls='nogps';   txt='&#9632; STANDBY'; subTxt='PRESS START NAVIGATION'; }

  if(state && state.indexOf('ARRIVED')<0 && state.indexOf('SET BASE')<0 && state.indexOf('STANDBY')<0){ tag.textContent='EN ROUTE'; tag.className='tag amber'; }
  if(state && state.indexOf('SET BASE')>=0){ tag.textContent='LOCKED'; tag.className='tag red'; }
  if(state && state.indexOf('STANDBY')>=0){ tag.textContent='STANDBY'; tag.className='tag'; }

  el.className = 'nav-state-text ' + cls;
  el.innerHTML = txt;
  sub.innerHTML = subTxt;
}

// ── Gyro bars ─────────────────────────────────────────────────
function setGyroBar(fillId, valId, val) {
  var maxRange = 4.0; // rad/s
  var fill = document.getElementById(fillId);
  var label = document.getElementById(valId);
  var pct = Math.min(Math.abs(val)/maxRange*50, 50);
  if(val >= 0){
    fill.style.left = '50%';
    fill.style.right = 'auto';
    fill.style.width = pct + '%';
  } else {
    fill.style.right = '50%';
    fill.style.left  = 'auto';
    fill.style.width = pct + '%';
  }
  label.textContent = (val >= 0 ? '+' : '') + val.toFixed(3);
}

// ── Satellite bars ─────────────────────────────────────────────
function buildSatBars(count) {
  var container = document.getElementById('satBars');
  container.innerHTML = '';
  var maxBars = 12;
  for(var i=0;i<maxBars;i++){
    var bar = document.createElement('div');
    bar.className = 'sat-bar' + (i < count ? ' lit' : '');
    bar.style.height = (10 + i*7/maxBars*14) + 'px';
    container.appendChild(bar);
  }
}

// ── Heading source tag ────────────────────────────────────────
function updateHdgSrc(src) {
  var el = document.getElementById('hdgSrcTag');
  var clean = (src||'').trim();
  el.className = 'hsrc ' + clean;
  el.textContent = 'HDG: ' + (clean||'NONE');
}

// ── Main update ───────────────────────────────────────────────
function updateUI(d) {
  if(!d) return;
  lastPacketAt = Date.now();

  var g   = d.gps   || {};
  var imu = d.imu   || {};
  var nav = d.nav   || {};
  var hr  = d.hr    || {};
  var ts  = d.timestamp || 0;

  // ── Connection status ──
  document.getElementById('connDot').className = 'dot live';
  document.getElementById('connText').textContent = 'LIVE — PKT #' + (d.packetNum||0);
  document.getElementById('pktTop').textContent  = d.packetNum || 0;
  document.getElementById('pktBig').textContent  = d.packetNum || 0;
  document.getElementById('dr-pkts').textContent = d.packetNum || 0;

  // ── GPS ──
  var gValid = g.valid;
  document.getElementById('g-lat').textContent = gValid ? (g.lat||0).toFixed(6) : '--';
  document.getElementById('g-lng').textContent = gValid ? (g.lng||0).toFixed(6) : '--';
  document.getElementById('g-alt').textContent = gValid ? (g.alt||0).toFixed(1)+' m' : '--';
  document.getElementById('g-spd').textContent = gValid ? (g.speed||0).toFixed(1)+' km/h' : '--';
  var sats = g.satellites || 0;
  document.getElementById('g-sats').textContent = sats + ' sats';
  document.getElementById('g-sats').className = 'gs-val ' + (sats>=6?'green':sats>=4?'amber':'red');
  buildSatBars(sats);
  var gpsTagEl = document.getElementById('gpsFixTag');
  gpsTagEl.textContent = gValid ? 'VALID FIX' : 'NO FIX';
  gpsTagEl.className = 'tag ' + (gValid ? 'green' : 'red');

  // ── IMU ──
  if(imu.valid){
    var ax = imu.accel_x||0, ay = imu.accel_y||0, az = imu.accel_z||0;
    var gx = imu.gyro_x||0,  gy = imu.gyro_y||0,  gz = imu.gyro_z||0;
    document.getElementById('a-x').textContent = ax.toFixed(2);
    document.getElementById('a-y').textContent = ay.toFixed(2);
    document.getElementById('a-z').textContent = az.toFixed(2);
    setGyroBar('gbx','gv-x',gx);
    setGyroBar('gby','gv-y',gy);
    setGyroBar('gbz','gv-z',gz);
    document.getElementById('imu-temp').textContent = (imu.temp||0).toFixed(1) + ' °C';
    document.getElementById('imu-bias').textContent = (nav.bias_z !== undefined ? nav.bias_z.toFixed(5) : '--');
    drawTilt(ax, ay);
    document.getElementById('imuTag').className = 'tag green';
    document.getElementById('imuTag').textContent = 'ACTIVE';
  } else {
    document.getElementById('imuTag').className = 'tag red';
    document.getElementById('imuTag').textContent = 'INACTIVE';
  }

  // ── Navigation ──
  var relBearing  = (nav.rel_bearing !== undefined) ? nav.rel_bearing : null;
  var bearing     = (nav.bearing     !== undefined) ? nav.bearing     : null;
  var heading     = (nav.fused_heading !== undefined)? nav.fused_heading : null;
  var dist        = (nav.distance    !== undefined) ? nav.distance    : null;
  var navState    = nav.state || '';
  var hdgSrc      = (nav.heading_src||'').trim();

  drawCompass(heading, bearing);
  updateArrow(relBearing, navState);
  updateNavState(navState, relBearing, dist);
  updateHdgSrc(hdgSrc);

  document.getElementById('cvHeading').textContent = heading !== null ? heading.toFixed(1)+'°' : '---°';
  document.getElementById('cvBearing').textContent = bearing !== null ? bearing.toFixed(1)+'°' : '---°';
  document.getElementById('cvRel').textContent     = relBearing !== null ? (relBearing>0?'+':'')+relBearing.toFixed(1)+'°' : '---°';
  document.getElementById('relBearLabel').textContent = relBearing !== null
    ? 'REL BEARING: ' + (relBearing>0?'+':'')+relBearing.toFixed(1)+'°'
    : 'REL BEARING: --';

  // Distance bar (max ref 25000m)
  var distRef = 25000;
  if(dist !== null){
    document.getElementById('distNum').textContent = dist > 1000
      ? (dist/1000).toFixed(2)+' km'
      : Math.round(dist);
    document.getElementById('distUnit') && (document.getElementById('distUnit').textContent = dist>1000 ? 'KM TO BASE' : 'METRES TO BASE');
    var pct = Math.max(0, Math.min(100, (1 - dist/distRef)*100));
    document.getElementById('distBar').style.width = pct + '%';
  }

  document.getElementById('dr-state').textContent   = navState.replace(/[<>*]/g,'').trim() || '--';
  document.getElementById('dr-bearing').textContent = bearing !== null ? bearing.toFixed(1)+'°' : '--';
  document.getElementById('dr-speed').textContent   = g.speed  !== undefined ? g.speed.toFixed(1)+' km/h' : '--';
  document.getElementById('dr-alt').textContent     = g.alt    !== undefined ? g.alt.toFixed(1)+' m' : '--';

  // ── Heart rate ──
  if(hr.valid){
    var fingerOn = hr.finger;
    var bpm      = hr.bpm || 0;
    document.getElementById('hrNum').textContent = fingerOn && bpm > 0 ? Math.round(bpm) : '---';
    document.getElementById('hrNum').className   = 'hr-num ' + (fingerOn && bpm>0 ? 'active' : 'inactive');
    document.getElementById('hrStatus').textContent = fingerOn ? '&#9632; FINGER DETECTED' : '&#9632; NO FINGER';
    document.getElementById('hrStatus').className   = 'hr-status ' + (fingerOn ? 'on' : 'off');
    document.getElementById('hrTag').className   = 'tag ' + (fingerOn?'red':'amber');
    document.getElementById('hrTag').textContent = fingerOn ? 'ACTIVE' : 'STANDBY';
    // Update history
    if(fingerOn && bpm > 0){
      hrHistory.push(bpm);
      hrHistory.shift();
    } else {
      hrHistory.push(0);
      hrHistory.shift();
    }
    drawHR(bpm, fingerOn);
  } else {
    document.getElementById('hrTag').className   = 'tag red';
    document.getElementById('hrTag').textContent = 'NOT FOUND';
    document.getElementById('hrNum').className   = 'hr-num inactive';
    document.getElementById('hrNum').textContent = 'N/A';
    document.getElementById('hrStatus').textContent = '&#9632; SENSOR NOT DETECTED';
    document.getElementById('hrStatus').className   = 'hr-status off';
    drawHR(0, false);
  }

  // ── Ticker ──
  var now = new Date();
  document.getElementById('tk-time').textContent  = now.toLocaleTimeString();
  document.getElementById('tk-gps').textContent   = gValid ? g.lat.toFixed(4)+','+g.lng.toFixed(4) : 'NO FIX';
  document.getElementById('tk-hdg').textContent   = heading !== null ? heading.toFixed(1)+'°' : '--';
  document.getElementById('tk-dist').textContent  = dist !== null ? (dist>1000?(dist/1000).toFixed(1)+'km':Math.round(dist)+'m') : '--';
  document.getElementById('tk-state').textContent = navState.replace(/[<>*&#;0-9A-Fa-f]/g,'').replace(/\s+/g,' ').trim() || '--';
  document.getElementById('tk-hr').textContent    = (hr.valid && hr.finger && hr.bpm>0) ? Math.round(hr.bpm)+' BPM' : 'NO FINGER';
  document.getElementById('tk-imu').textContent   = imu.valid ? 'OK T:'+((imu.temp||0).toFixed(1))+'C' : 'INACTIVE';
  var upSec = Math.floor((Date.now()-startedAt)/1000);
  document.getElementById('tk-uptime').textContent = Math.floor(upSec/60)+'m '+( upSec%60)+'s';

  // ── Raw JSON ──
  document.getElementById('rawJson').textContent = JSON.stringify(d, null, 2);

  // ── Base station coords + navigation lock status ──
  if (d.base_lat !== undefined) {
    document.getElementById('base-cur-lat').textContent = (+d.base_lat).toFixed(6);
    document.getElementById('base-cur-lng').textContent = (+d.base_lng).toFixed(6);
  }
  if (d.base_confirmed !== undefined) {
    var bTag = document.getElementById('baseSetTag');
    var bPanel = document.getElementById('baseSetPanel');
    if (d.base_confirmed) {
      bTag.textContent = 'NAV ACTIVE'; bTag.className = 'tag green';
      if (bPanel) bPanel.style.borderColor = 'rgba(0,230,118,.4)';
    } else {
      bTag.textContent = 'SET REQUIRED'; bTag.className = 'tag red';
      if (bPanel) bPanel.style.borderColor = 'rgba(255,61,87,.4)';
    }
  }
  if (d.nav_enabled !== undefined && d.nav_enabled !== navRunning) {
    setNavRunning(d.nav_enabled);
  }
}

// ── Age updater ───────────────────────────────────────────────
setInterval(function(){
  if(!lastPacketAt) return;
  var age = (Date.now()-lastPacketAt)/1000;
  document.getElementById('ageTop').textContent = age.toFixed(1)+'s';
  document.getElementById('dr-age').textContent = age.toFixed(1)+'s ago';
  if(age > 6){
    document.getElementById('connDot').className = 'dot warn';
    document.getElementById('connText').textContent = 'STALE — '+age.toFixed(1)+'s ago';
  }
}, 500);

// ── Polling ───────────────────────────────────────────────────
setInterval(function(){
  fetch('/api/data',{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(updateUI)
    .catch(function(){
      document.getElementById('connDot').className = 'dot';
      document.getElementById('connText').textContent = 'DISCONNECTED';
    });
}, 2000);

// ── Navigation start/stop ────────────────────────────────
var navRunning = false;
function setNavRunning(active) {
  navRunning = active;
  document.getElementById('navStandby').style.display = active ? 'none' : 'flex';
  document.getElementById('navActive').style.display  = active ? 'flex' : 'none';
}
function startNav() {
  fetch('/api/navstart', {method:'POST', headers:{'Content-Type':'application/json'}, body:'{}'})
    .then(function(r){return r.json()})
    .then(function(d){ if(d.ok) setNavRunning(true); })
    .catch(function(){ /* offline demo */ setNavRunning(true); });
}
function stopNav() {
  fetch('/api/navstop', {method:'POST', headers:{'Content-Type':'application/json'}, body:'{}'})
    .then(function(r){return r.json()})
    .then(function(d){ if(d.ok) setNavRunning(false); })
    .catch(function(){ setNavRunning(false); });
}

// ── Set base station coordinates ─────────────────────────────
function setBase() {
  var lat = parseFloat(document.getElementById('inp-lat').value);
  var lng = parseFloat(document.getElementById('inp-lng').value);
  var msg = document.getElementById('base-msg');
  if (isNaN(lat) || isNaN(lng) || lat < -90 || lat > 90 || lng < -180 || lng > 180) {
    msg.style.color = '#ff3d57';
    msg.textContent = 'INVALID COORDINATES';
    return;
  }
  msg.style.color = '#ffab00';
  msg.textContent = 'SENDING...';
  fetch('/api/setbase', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({lat: lat, lng: lng})
  })
  .then(function(r) { return r.json(); })
  .then(function(d) {
    if (d.ok) {
      msg.style.color = '#00e676';
      msg.textContent = 'BASE SET ✓  ' + lat.toFixed(6) + ', ' + lng.toFixed(6);
      document.getElementById('base-cur-lat').textContent = lat.toFixed(6);
      document.getElementById('base-cur-lng').textContent = lng.toFixed(6);
      document.getElementById('inp-lat').value = '';
      document.getElementById('inp-lng').value = '';
      var bTag = document.getElementById('baseSetTag');
      var bPanel = document.getElementById('baseSetPanel');
      if(bTag){ bTag.textContent='NAV ACTIVE'; bTag.className='tag green'; }
      if(bPanel) bPanel.style.borderColor='rgba(0,230,118,.4)';
    } else {
      msg.style.color = '#ff3d57';
      msg.textContent = 'ERROR: ' + (d.error || 'unknown');
    }
  })
  .catch(function() {
    msg.style.color = '#ff3d57';
    msg.textContent = 'FAILED — CHECK CONNECTION';
  });
}

// ── Initial draw (empty state) ────────────────────────────────
drawCompass(null, null);
drawTilt(0, 0);
drawHR(0, false);
buildSatBars(0);
</script>
</body>
</html>
)rawliteral";

// =====================================================================
//  HTTP HELPERS
// =====================================================================
const char* httpMethodName(HTTPMethod method) {
  switch (method) {
    case HTTP_GET:     return "GET";
    case HTTP_POST:    return "POST";
    case HTTP_PUT:     return "PUT";
    case HTTP_DELETE:  return "DELETE";
    case HTTP_OPTIONS: return "OPTIONS";
    default:           return "OTHER";
  }
}

void logHttpRequest() {
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
  StaticJsonDocument<900> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[HTTP] JSON parse error: %s\n", err.c_str());
    return false;
  }
  packetsReceived++;
  doc["timestamp"]  = millis();
  doc["packetNum"]  = packetsReceived;
  doc["base_lat"]       = storedBaseLat;
  doc["base_lng"]       = storedBaseLng;
  doc["base_confirmed"] = baseLocationConfirmed;
  doc["nav_enabled"]    = navEnabled;
  latestDataStr = "";
  serializeJson(doc, latestDataStr);
  lastPacketMs = millis();
  digitalWrite(STATUS_LED, HIGH);

  // ── Pretty-print packet to Serial ──────────────────────────
  JsonObject nav = doc["nav"];
  JsonObject gps = doc["gps"];
  JsonObject imu = doc["imu"];
  Serial.println("════════════════════════════════════════");
  Serial.printf ("  PKT #%d\n", packetsReceived);
  Serial.println("────────────────────────────────────────");
  Serial.printf ("  Heading : %.1f deg   Bearing : %.1f deg\n",
                 (float)nav["fused_heading"], (float)nav["bearing"]);
  Serial.printf ("  Rel Ang : %.1f deg   Distance: %.1f m\n",
                 (float)nav["rel_bearing"], (float)nav["distance"]);
  Serial.printf ("  State   : %s\n", (const char*)nav["state"]);
  Serial.println("────────────────────────────────────────");
  if ((bool)gps["valid"]) {
    Serial.printf ("  GPS     : %.6f, %.6f  Sats: %d  Spd: %.1f km/h\n",
                   (double)gps["lat"], (double)gps["lng"],
                   (int)gps["satellites"], (float)gps["speed"]);
  } else {
    Serial.println("  GPS     : NO FIX");
  }
  Serial.printf ("  Accel   : X=%.2f  Y=%.2f  Z=%.2f  m/s²\n",
                 (float)imu["accel_x"], (float)imu["accel_y"], (float)imu["accel_z"]);
  Serial.printf ("  Gyro Z  : %.3f deg/s   Temp: %.1f C\n",
                 (float)imu["gyro_z"], (float)imu["temp"]);
  Serial.println("════════════════════════════════════════");

  return true;
}

void sendApiData() {
  logHttpRequest();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");

  if (server.method() == HTTP_OPTIONS) { server.send(204,"text/plain",""); return; }

  if (server.method() == HTTP_GET) {
    // Always inject fresh base coords (may have changed since last POST)
    StaticJsonDocument<1200> tmp;
    deserializeJson(tmp, latestDataStr);
    tmp["base_lat"]       = storedBaseLat;
    tmp["base_lng"]       = storedBaseLng;
    tmp["base_confirmed"] = baseLocationConfirmed;
    tmp["nav_enabled"]    = navEnabled;
    String out;
    serializeJson(tmp, out);
    server.send(200, "application/json", out);
    return;
  }

  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    if (updateFromJson(body)) {
      Serial.printf("[HTTP] Packet #%d (%d bytes)\n", packetsReceived, body.length());
      server.send(200, "application/json", latestDataStr);
    } else {
      server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
    }
    return;
  }
  server.send(405, "text/plain", "Method Not Allowed");
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  unsigned long ws = millis();
  while (!Serial && millis()-ws < 5000) delay(10);

  pinMode(STATUS_LED, OUTPUT);

  Serial.println("\n\n=== BASE STATION 2 — TACTICAL OPS ===");

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, 1, 0, 4);
  Serial.printf("AP: %s  IP: %s  SSID: %s\n",
    apOk?"OK":"FAIL", WiFi.softAPIP().toString().c_str(), AP_SSID);

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/",              HTTP_ANY, [](){sendDashboard();});
  server.on("/index.html",    HTTP_ANY, [](){sendDashboard();});
  server.on("/api/data",      HTTP_ANY, [](){sendApiData();});
  server.on("/api/setbase",   HTTP_ANY, [](){
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    if (server.method() == HTTP_OPTIONS) { server.send(204,"text/plain",""); return; }
    if (server.method() != HTTP_POST)    { server.send(405,"text/plain","Method Not Allowed"); return; }
    String body = server.arg("plain");
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, body) || !doc.containsKey("lat") || !doc.containsKey("lng")) {
      server.send(400,"application/json","{\"error\":\"invalid_json\"}");
      return;
    }
    double lat = doc["lat"];
    double lng = doc["lng"];
    if (lat < -90 || lat > 90 || lng < -180 || lng > 180) {
      server.send(400,"application/json","{\"error\":\"out_of_range\"}");
      return;
    }
    storedBaseLat = lat;
    storedBaseLng = lng;
    baseLocationConfirmed = true;
    Serial.printf("[BASE] Coordinates confirmed: %.6f, %.6f \u2014 navigation enabled\n", storedBaseLat, storedBaseLng);
    String resp = "{\"ok\":true,\"lat\":" + String(storedBaseLat, 6) + ",\"lng\":" + String(storedBaseLng, 6) + "}";
    server.send(200, "application/json", resp);
  });
  server.on("/api/navstart", HTTP_ANY, [](){
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    if (server.method() == HTTP_OPTIONS) { server.send(204,"text/plain",""); return; }
    navEnabled = true;
    Serial.println("[NAV] Navigation STARTED by dashboard");
    server.send(200, "application/json", "{\"ok\":true,\"nav_enabled\":true}");
  });
  server.on("/api/navstop", HTTP_ANY, [](){
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    if (server.method() == HTTP_OPTIONS) { server.send(204,"text/plain",""); return; }
    navEnabled = false;
    Serial.println("[NAV] Navigation STOPPED by dashboard");
    server.send(200, "application/json", "{\"ok\":true,\"nav_enabled\":false}");
  });
  server.on("/generate_204",  HTTP_GET, [](){server.send(204,"text/plain","");});
  server.on("/gen_204",       HTTP_GET, [](){server.send(204,"text/plain","");});
  server.on("/hotspot-detect.html", HTTP_GET, [](){
    server.send(200,"text/html","<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/connecttest.txt", HTTP_GET, [](){
    server.send(200,"text/plain","Microsoft Connect Test");
  });
  server.onNotFound([](){
    logHttpRequest();
    if (server.method()==HTTP_GET){ sendDashboard(); return; }
    server.send(404,"text/plain","Not Found");
  });

  server.begin();
  Serial.println("Dashboard ready → http://192.168.4.1/");
  Serial.println("Waiting for belt data...\n");
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
  static unsigned long ledOnTime = 0;
  if (digitalRead(STATUS_LED)) {
    if (!ledOnTime) ledOnTime = millis();
    if (millis()-ledOnTime > 100) { digitalWrite(STATUS_LED, LOW); ledOnTime = 0; }
  }

  dnsServer.processNextRequest();
  server.handleClient();

  static unsigned long lastHB = 0;
  if (millis()-lastHB > 10000) {
    Serial.printf("[ALIVE] Up:%lus | Pkts:%d | Clients:%d | LastPkt:%lus ago\n",
      millis()/1000, packetsReceived, WiFi.softAPgetStationNum(),
      lastPacketMs ? (millis()-lastPacketMs)/1000 : 0);
    lastHB = millis();
  }
  delay(5);
}