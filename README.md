# 🎯 Belt Monitoring System

A modern IoT solution for belt monitoring using ESP32, GPS, IMU sensors, and wireless communication. This project offers **two complete setups**:

## 📡 Two Setup Options

### 🔷 Option 1: LoRa Setup (Long Range)
**Files:** `base_station/` + `belt/` + `example/`  
**Range:** Up to 10km line-of-sight  
**Use Case:** Outdoor tracking, long-distance monitoring  
**Hardware:** Requires RA-02 LoRa modules  
**Documentation:** This README

### 🔷 Option 2: WiFi Setup (Enhanced Dashboard)
**Files:** `base_station2/` + `belt2/`  
**Range:** ~100m indoors, 300m outdoors  
**Use Case:** Indoor use, testing, close-range monitoring  
**Hardware:** ESP32 only (no LoRa modules needed!)  
**Dashboard:** Enhanced with interactive maps & charts  
**Documentation:** See **[WIFI_SETUP.md](WIFI_SETUP.md)** ⭐

---

## 🏗️ LoRa Architecture (Option 1)

**Belt Device (ESP32)** → **LoRa** → **Base Station (ESP32)** → **WiFi AP** → **Web Dashboard (HTML/CSS/JS)**

- **Belt Device**: Collects sensor data and transmits via LoRa
- **Base Station**: Receives data, creates WiFi network, provides WebSocket & REST API
- **Frontend Dashboard**: Beautiful web interface that connects to the base station

## 🌟 Features

### Hardware Integration
- **LoRa Communication**: Long-range wireless data transmission (RA-02 433MHz modules)
- **GPS Tracking**: Real-time location monitoring (NEO-M8N)
- **Motion Sensing**: 6-axis IMU for acceleration and gyroscope data (MPU6050)
- **Motor Control**: Dual motor control system (L298N driver)

### Backend (ESP32 Base Station)
- ✅ WiFi Access Point (No internet required!)
- ✅ WebSocket server for real-time data streaming
- ✅ RESTful API endpoints (`/api/data`, `/api/history`, `/api/info`)
- ✅ Historical data buffer (50 packets)
- ✅ CORS enabled for frontend access

### Frontend (Web Dashboard)
- 🎨 Beautiful modern UI with gradient design
- 📱 Responsive (works on mobile, tablet, desktop)
- ⚡ Real-time data updates via WebSocket
- 📊 GPS, IMU, Gyroscope, and Motor status displays
- 📡 Signal quality monitoring
- 🔌 Auto-reconnect on disconnect
- 🎯 Easy to customize and extend

## 📁 Project Structure

```
belt-project/
├── src/
│   ├── base_station/        # LoRa Base Station (Option 1)
│   │   └── main.cpp         # LoRa RX, WiFi AP, API server
│   ├── belt/                # LoRa Belt Device (Option 1)
│   │   └── main.cpp         # Sensors, LoRa TX
│   ├── example/             # Haptic Navigation Example
│   │   └── main.cpp         # Vibration-based navigation
│   ├── base_station2/       # WiFi Base Station (Option 2) ⭐
│   │   └── main.cpp         # WiFi AP, Enhanced Dashboard
│   └── belt2/               # WiFi Belt Device (Option 2) ⭐
│       └── main.cpp         # Sensors, WiFi client
├── frontend/                # Web Dashboard Files
│   ├── index.html           # Basic dashboard
│   ├── styles.css           # Styling
│   ├── app.js               # WebSocket logic
│   ├── enhanced.html        # Enhanced dashboard (standalone)
│   ├── enhanced-styles.css  # Enhanced styling
│   └── enhanced-app.js      # Enhanced functionality
├── platformio.ini           # PlatformIO configuration
├── README.md               # This file (LoRa setup)
└── WIFI_SETUP.md           # WiFi setup guide ⭐
```

## 🚀 Quick Start (LoRa Setup)

### Step 1: Hardware Setup
Wire all components according to pin definitions in the code

### Step 2: Upload ESP32 Firmware
```bash
# Upload to belt device
pio run -e belt -t upload

# Upload to base station
pio run -e base_station -t upload
```

### Step 3: Open Frontend Dashboard

**🎯 Easiest Method (Recommended):**
1. Connect to WiFi: **"BeltStation"** (password: **belt12345**)
2. Navigate to `frontend/` folder on your computer
3. **Double-click `index.html`** to open in your browser
4. See real-time data! 🎉

**Alternative Methods:**
- **Python:** `cd frontend && python -m http.server 8080` then open http://localhost:8080
- **Node.js:** Install http-server globally, then run in frontend folder
- **VS Code:** Install "Live Server" extension, right-click index.html → "Open with Live Server"

See [frontend/README.md](frontend/README.md) for detailed instructions.

## 📡 API Endpoints

The ESP32 base station provides these endpoints at `http://192.168.4.1`:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Simple landing page with API info |
| `/api/data` | GET | Latest sensor data (JSON) |
| `/api/history` | GET | Historical data - last 50 packets (JSON array) |
| `/api/info` | GET | System information (uptime, packets count, etc.) |
| `ws://192.168.4.1/ws` | WebSocket | Real-time data stream |

### Example API Response (`/api/data`):
```json
{
  "gps": {
    "valid": true,
    "lat": 37.422408,
    "lng": -122.084068,
    "alt": 25.5,
    "speed": 1.2,
    "sats": 8
  },
  "imu": {
    "valid": true,
    "accel": {"x": 0.12, "y": 0.05, "z": 9.81},
    "gyro": {"x": 0.01, "y": -0.02, "z": 0.00},
    "temp": 24.5
  },
  "motors": {
    "left_speed": 128,
    "left_dir": "forward",
    "right_speed": 128,
    "right_dir": "forward"
  },
  "rssi": -65,
  "snr": 8.5,
  "uptime": 123456,
  "timestamp": 123456
}
```

## 🎨 Customization

### Customize Dashboard Look & Feel
All dashboard files are in the `frontend/` folder - super easy to customize!

**Change Colors:**
Edit `frontend/styles.css` (lines 10-17):
```css
:root {
    --primary-color: #667eea;      /* Main color */
    --secondary-color: #764ba2;    /* Accent color */
    --success-color: #4CAF50;      /* Success indicators */
    --danger-color: #f44336;       /* Error indicators */
}
```

**Add New Data Fields:**
1. Add HTML element in `frontend/index.html`
2. Update JavaScript in `frontend/app.js` to populate it

**Detailed Guide:** See [frontend/README.md](frontend/README.md)

### Change WiFi Credentials
Edit `src/base_station/main.cpp`:
```cpp
const char* AP_SSID = "YourNetworkName";
const char* AP_PASSWORD = "your_password";
```

### Change ESP32 IP Address
If you need to use a different IP, update in the dashboard settings panel or edit `frontend/app.js`:
```javascript
let ESP32_IP = '192.168.4.1';  // Change here
```

## 🛠️ Hardware Requirements

### Belt Device
- ESP32 DevKit v1
- LoRa Module RA-02 (433 MHz)
- GPS Module NEO-M8N
- IMU MPU6050
- Motor Driver L298N
- 2x DC Motors

### Base Station
- ESP32 DevKit v1
- LoRa Module RA-02 (433 MHz)

See [SIMPLE_SETUP.md](SIMPLE_SETUP.md) for detailed wiring instructions.

## 📚 Software Dependencies

All dependencies are automatically installed by PlatformIO:

- **Arduino Framework**
- **LoRa** by Sandeep Mistry (v0.8.0)
- **TinyGPSPlus** (v1.0.3)
- **Adafruit MPU6050** (v2.2.4)
- **ArduinoJson** (v6.21.3)
- **ESPAsyncWebServer** (v1.2.3)

## 🐛 Troubleshooting

### Dashboard Won't Connect
- ✅ Check ESP32 base station is powered on
- ✅ Verify you're connected to "BeltStation" WiFi
- ✅ Try http://192.168.4.1 in browser address bar
- ✅ Check browser console (F12) for errors
- ✅ Press 'R' key to manually reconnect WebSocket

### No Data Showing
- ✅ Check belt device is powered on
- ✅ Monitor ESP32 serial output (115200 baud)
- ✅ Verify LoRa antennas are connected
- ✅ Check sensor wiring per SIMPLE_SETUP.md

### GPS Not Working
- ✅ GPS needs clear view of sky
- ✅ First fix can take 1-5 minutes
- ✅ Check GPS baud rate is 9600
- ✅ Verify TX/RX pins are correct (GPIO 16/17)

## 📖 Documentation

- **[SIMPLE_SETUP.md](SIMPLE_SETUP.md)** - Hardware wiring and ESP32 setup
- **[frontend/README.md](frontend/README.md)** - Frontend customization guide

## 🎯 Use Cases

- Worker safety monitoring
- Motion tracking and analysis
- Vehicle/equipment monitoring
- Remote asset tracking
- Sports performance analysis
- Industrial automation

## 🔮 Future Enhancements

- [ ] Add actual GPS map with location tracking
- [ ] Historical data charts (Chart.js integration)
- [ ] Export data to CSV
- [ ] Alert/notification system
- [ ] Dark mode toggle
- [ ] Multiple belt device support
- [ ] SD card data logging
- [ ] Battery level monitoring

## 🌟 Want WiFi Setup Instead?

**Check out [WIFI_SETUP.md](WIFI_SETUP.md) for the WiFi-only version!**

✅ No LoRa modules needed  
✅ Enhanced dashboard with maps & charts  
✅ Easier setup for testing  
✅ Perfect for indoor use  

## 📝 License

This project is open source. Feel free to use, modify, and distribute.

## 🤝 Contributing

Contributions welcome! Feel free to:
- Report bugs
- Suggest features
- Submit pull requests
- Improve documentation

---

**Made with ❤️ using ESP32, LoRa, and Arduino**
