# 📡 WiFi Setup - Base Station 2 & Belt 2

**Complete WiFi-based Belt Monitoring System (No LoRa Required!)**

## 🎯 Overview

This is the **WiFi-only version** of the belt monitoring system. Unlike the LoRa version, both devices communicate over WiFi:

- **Base Station 2**: Creates WiFi Access Point + Hosts Enhanced Dashboard
- **Belt 2**: Connects to base station WiFi + Sends sensor data via HTTP

**Perfect for:** Close-range applications, indoor use, testing without LoRa modules

## 🏗️ Architecture

```
┌─────────────────────────────┐
│   BASE STATION 2 (ESP32)    │
│  • Creates WiFi AP          │
│  • Receives HTTP POST data  │
│  • Hosts Enhanced Dashboard │
│  • WebSocket broadcasting   │
│  IP: 192.168.4.1           │
└─────────────────────────────┘
           ↓ WiFi Network
    ┌──────┴─────┬────────────┐
    ↓            ↓            ↓
┌─────────┐  ┌──────────┐  ┌──────────┐
│ BELT 2  │  │ LAPTOP/  │  │  PHONE   │
│ ESP32   │  │ COMPUTER │  │  TABLET  │
│         │  │          │  │          │
│ Sensors │  │Dashboard │  │Dashboard │
└─────────┘  └──────────┘  └──────────┘
```

## ✨ Features

### Base Station 2
- ✅ WiFi Access Point (SSID: "BeltStation")
- ✅ Enhanced dashboard with maps & charts
- ✅ No external CDN dependencies needed (loads from internet)
- ✅ Real-time WebSocket updates
- ✅ HTTP POST endpoint for receiving data
- ✅ Serves dashboard at `http://192.168.4.1/`

### Belt 2
- ✅ WiFi client (connects to base station)
- ✅ GPS tracking (NEO-M8N)
- ✅ IMU motion sensing (MPU6050)
- ✅ Sends data every 2 seconds via HTTP POST
- ✅ Auto-reconnects if WiFi drops

### Enhanced Dashboard
- 🗺️ **Interactive GPS Map** (Leaflet.js) with path tracking
- 📊 **Live Charts** (Chart.js) for accelerometer & gyroscope
- 🧭 **Compass Visualization** for navigation
- 📱 **Responsive Design** - works on any device
- 🎨 **Modern UI** with dark theme

## 🔧 Hardware Setup

### Base Station 2 (ESP32 #1)
**Minimal setup - No sensors needed!**

| Component | Pin | Notes |
|-----------|-----|-------|
| Status LED | GPIO 2 | Built-in LED |
| USB Power | - | Can use USB power bank |

**That's it!** No LoRa, no sensors required.

### Belt 2 (ESP32 #2)
**Sensors only - No LoRa!**

| Component | Pin | Connection |
|-----------|-----|------------|
| **GPS NEO-M8N** | | |
| GPS TX | GPIO 16 (ESP RX) | Yellow wire |
| GPS RX | GPIO 17 (ESP TX) | White wire |
| GPS VCC | 3.3V | Red wire |
| GPS GND | GND | Black wire |
| **IMU MPU6050** | | |
| SDA | GPIO 21 | I2C Data |
| SCL | GPIO 22 | I2C Clock |
| VCC | 3.3V | |
| GND | GND | |
| **Status** | | |
| LED | GPIO 2 | Built-in LED |

## 📥 Installation & Upload

### 1. Upload Base Station 2

```bash
# Connect Base Station ESP32
pio run -e base_station2 -t upload
```

**What it does:**
- Creates WiFi AP "BeltStation" (password: belt12345)
- Starts web server at 192.168.4.1
- Waits for belt device data

### 2. Upload Belt 2

```bash
# Connect Belt ESP32
pio run -e belt2 -t upload
```

**What it does:**
- Connects to "BeltStation" WiFi
- Reads GPS & IMU sensors
- Sends data to base station every 2 seconds

### 3. Monitor Output (Optional)

**Base Station:**
```bash
pio device monitor -e base_station2
```

**Belt Device:**
```bash
pio device monitor -e belt2
```

## 🚀 Usage

### Quick Start (5 Steps!)

1. **Power on Base Station ESP32**
   - Wait ~5 seconds for WiFi AP to start
   - Look for "BeltStation" WiFi network

2. **Power on Belt ESP32**
   - It will auto-connect to base station
   - Status LED blinks when sending data

3. **Connect your phone/laptop to WiFi**
   - Network: **BeltStation**
   - Password: **belt12345**

4. **Open browser and visit**
   - URL: `http://192.168.4.1/`
   - Enhanced dashboard loads automatically!

5. **See live data!**
   - GPS map updates in real-time
   - Charts show sensor data
   - Wait 1-5 minutes for GPS fix outdoors

### Serial Monitor Output

**Base Station 2:**
```
╔══════════════════════════════════════════════╗
║    BASE STATION 2 - WiFi Only                ║
║    Enhanced Dashboard with WiFi Communication║
╚══════════════════════════════════════════════╝

[WiFi] Configuring Access Point...
✅ AP IP address: 192.168.4.1
   SSID: BeltStation
   Password: belt12345
   Dashboard: http://192.168.4.1/
✅ Web server started

✅ Base Station 2 Ready!

📱 To connect:
   1. Connect to WiFi: BeltStation (password: belt12345)
   2. Open browser: http://192.168.4.1/
   3. View enhanced dashboard!
```

**Belt 2:**
```
╔══════════════════════════════════════════════╗
║    BELT DEVICE 2 - WiFi Only                 ║
║    Connects to base station via WiFi         ║
╚══════════════════════════════════════════════╝

[WiFi] Connecting to Base Station...
   SSID: BeltStation
✅ WiFi Connected!
   IP Address: 192.168.4.2
✅ GPS initialized
✅ MPU6050 initialized

✅ Belt Device 2 Ready!

╔═══════════════════════════════════════╗
║      SENSOR STATUS (WiFi Mode)       ║
╚═══════════════════════════════════════╝
WiFi:          ✅ Connected (192.168.4.2)
GPS:           ✅ Valid (8 sats)
  Location:    37.422408, -122.084068
IMU:           ✅ Active
[HTTP] Data sent successfully! (Packet #1)
```

## 📊 Dashboard Features

### 🗺️ GPS Map
- **Live marker** shows your current position
- **Path trail** shows where you've been (last 100 points)
- **Dark theme** map tiles
- **Auto-centering** on your location

### 📈 Live Charts
- **Accelerometer**: X/Y/Z axes in real-time
- **Gyroscope**: Rotation data
- **Color-coded**: Red (X), Green (Y), Blue (Z)
- **Smooth animations**

### 🧭 Navigation Card (if using example code)
- Distance to base station
- Bearing & heading
- Visual compass
- Direction indicators

### ⚡ System Info
- Temperature from IMU
- Packets received counter
- Last update timestamp

## 🔧 Configuration

### Change WiFi Credentials

Edit `src/base_station2/main.cpp`:
```cpp
#define AP_SSID              "YourNetworkName"
#define AP_PASSWORD          "YourPassword123"
```

Edit `src/belt2/main.cpp`:
```cpp
#define WIFI_SSID           "YourNetworkName"
#define WIFI_PASSWORD       "YourPassword123"
```

### Change Data Send Interval

Edit `src/belt2/main.cpp`:
```cpp
#define SENSOR_TRANSMIT_INTERVAL 2000   // milliseconds (2 seconds)
```

Change to `1000` for 1-second updates, or `5000` for 5-second updates.

## 📱 Using on Mobile Devices

### iPhone/iPad
1. Settings → WiFi
2. Connect to "BeltStation"
3. Enter password: belt12345
4. Open Safari
5. Go to: `http://192.168.4.1/`
6. Tap "Add to Home Screen" for app-like experience!

### Android
1. Settings → WiFi
2. Connect to "BeltStation"
3. Enter password: belt12345
4. Open Chrome
5. Go to: `http://192.168.4.1/`
6. Menu → "Add to Home screen"

## 🆚 WiFi vs LoRa Comparison

| Feature | WiFi (base_station2/belt2) | LoRa (base_station/belt) |
|---------|---------------------------|-------------------------|
| **Range** | ~100m indoors, 300m outdoors | Up to 10km line-of-sight |
| **Hardware** | ESP32 only (no LoRa modules) | Requires RA-02 LoRa modules |
| **Speed** | Fast (HTTP POST) | Slower (radio transmission) |
| **Power** | Higher consumption | Lower power for belt |
| **Cost** | Cheaper (no LoRa modules) | More expensive |
| **Dashboard** | Enhanced with maps/charts | Basic dashboard |
| **Use Case** | Indoor, testing, close-range | Outdoor, long-range |

## 🐛 Troubleshooting

### Belt won't connect to WiFi
- ✅ Check base station is powered on first
- ✅ Verify WiFi credentials match in both codes
- ✅ Check Serial Monitor for connection status
- ✅ Move belt closer to base station

### Dashboard not loading
- ✅ Confirm you're connected to "BeltStation" WiFi
- ✅ Try `http://192.168.4.1/` (not https)
- ✅ Clear browser cache
- ✅ Check base station Serial Monitor

### No GPS data on dashboard
- ✅ GPS needs clear sky view (go outdoors!)
- ✅ Wait 1-5 minutes for first fix
- ✅ Check GPS wiring (TX/RX might be swapped)
- ✅ Verify GPS has power (3.3V)

### Dashboard shows "Disconnected"
- ✅ Belt device might not be sending data
- ✅ Check belt Serial Monitor for errors
- ✅ Verify belt is connected to WiFi
- ✅ Refresh browser page

### Charts not showing
- ✅ Need internet connection for Chart.js CDN to load
- ✅ Or wait for libraries to cache
- ✅ Check browser console for errors

## 🚀 Next Steps

### Add More Features
1. **Battery monitoring** - Add voltage sensor
2. **Multiple belts** - Support multiple belt devices
3. **Data logging** - Save to SD card
4. **Alerts** - Send notifications on events
5. **Offline maps** - Embed map tiles in ESP32

### Extend the Dashboard
The dashboard HTML is embedded but can be customized:
- Edit `src/base_station2/main.cpp`
- Find the `dashboard_html` section
- Modify colors, layout, features
- Re-upload to ESP32

## 📚 Related Files

- **LoRa Version**: `src/base_station/` & `src/belt/` (original LoRa communication)
- **Haptic Navigation**: `src/example/` (vibration-based navigation)
- **Main README**: See root `README.md` for LoRa setup

## 💡 Tips

- **Testing**: Upload base_station2 first, then belt2
- **Range**: Keep devices within ~50-100m for best results
- **Power**: Use power banks for portable operation
- **GPS**: Always test outdoors for GPS lock
- **Dashboard**: Works best on Chrome/Safari
- **Development**: Use Serial Monitor to debug issues

## ⚡ Quick Reference

**Upload Commands:**
```bash
pio run -e base_station2 -t upload   # Upload base station
pio run -e belt2 -t upload           # Upload belt device
```

**WiFi Network:**
- SSID: `BeltStation`
- Password: `belt12345`
- Dashboard: `http://192.168.4.1/`

**Data Format:**
```json
{
  "gps": {"lat": 37.42, "lng": -122.08, "satellites": 8},
  "imu": {"accel_x": 0.1, "accel_y": 0.2, "accel_z": 9.8},
  "gyro": {"x": 0.0, "y": 0.0, "z": 0.0}
}
```

---

**🎉 You now have a complete WiFi-based belt monitoring system!**

*No LoRa modules needed - just two ESP32 boards and sensors!*
