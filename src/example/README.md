# 🧭 Haptic Navigation Belt - Example

A **vibration-based navigation system** that guides you to a destination using tactile feedback!

## 🎯 How It Works

1. **GPS tracks your location** - Knows where you are
2. **Calculates direction to base station** - Knows where you need to go
3. **Vibrates the correct motor** - Shows which way to turn
4. **Updates as you rotate** - Vibration switches to the motor pointing toward your destination

## 💡 Concept

Think of it like a **compass you can feel**:
- Base station is **ahead** → **FRONT motor** vibrates
- Base station is **behind** → **BACK motor** vibrates  
- Base station is **to your left** → **LEFT motor** vibrates
- Base station is **to your right** → **RIGHT motor** vibrates

As you rotate your body, the vibration automatically switches to the correct motor!

## 🔧 Setup

### 1. Configure Base Station Location

Edit `src/example/main.cpp` lines 65-66:

```cpp
#define BASE_STATION_LAT    37.422408   // YOUR base station latitude
#define BASE_STATION_LNG    -122.084068 // YOUR base station longitude
```

**How to get coordinates:**
- Open Google Maps
- Right-click on base station location
- Click on the coordinates to copy them
- Paste into the code

### 2. Upload to ESP32

```bash
pio run -e example -t upload
```

### 3. Monitor Serial Output

```bash
pio device monitor -e example
```

## 📊 What You'll See

```
╔═══════════════════════════════════════╗
║      HAPTIC NAVIGATION STATUS         ║
╚═══════════════════════════════════════╝
Current Position:  37.385000, -122.050000
Base Station:      37.422408, -122.084068
Distance to base:  5234.5 m
Bearing to base:   315.2°
Current heading:   0.0°
Relative bearing:  315.2° → LEFT

↓ LEFT motor vibrating
```

## 🎮 Motor Layout

```
         FRONT
           |
    LEFT --|-- RIGHT
           |
         BACK
```

## ⚙️ How Motors Work

The 4 motors are mounted on:
- **Front**: On your chest/front of belt
- **Back**: On your back
- **Left**: On your left side
- **Right**: On your right side

Each motor vibrates with a **pulse pattern**:
- Vibration duration: 300ms
- Interval: 1 second
- Strength: Adjustable (0-255)

## 🔄 Bearing Calculation

The system calculates:

1. **Bearing to base** - What direction (0-360°) is the base station from you?
2. **Current heading** - Which direction (0-360°) are you facing?
3. **Relative bearing** - The difference between the two

Example:
- Bearing to base: 315° (northwest)
- Your heading: 0° (facing north)
- Relative bearing: 315° → **LEFT motor vibrates**

## 📐 Direction Zones

- **FRONT**: -45° to +45° (ahead of you)
- **RIGHT**: 45° to 135° (to your right)
- **BACK**: 135° to 225° (behind you)
- **LEFT**: 225° to 315° (to your left)

## 🎯 Use Cases

✅ **Navigation without looking at phone**
- Guide users to a specific location
- Eyes-free navigation for accessibility

✅ **Search and rescue**
- Guide rescue teams to target location
- Works in low visibility conditions

✅ **Sports/Training**
- Guide runners back to starting point
- Create navigation-based training exercises

✅ **Accessibility**
- Help visually impaired navigate
- Provide directional awareness

## 🔧 Customization

### Adjust Vibration Strength
Edit line 68 in `main.cpp`:
```cpp
#define VIBRATION_STRENGTH  200  // 0-255 (higher = stronger)
```

### Change Pulse Pattern
Lines 69-70:
```cpp
#define VIBRATION_PULSE_MS  300   // Duration of each pulse
#define VIBRATION_INTERVAL  1000  // Time between pulses
```

### Adjust Direction Tolerance
Line 73:
```cpp
#define DIRECTION_TOLERANCE 45    // ±45° for each motor
```

## 📡 Data Transmitted via LoRa

The system also transmits navigation data:

```json
{
  "nav": {
    "current_lat": 37.385000,
    "current_lng": -122.050000,
    "base_lat": 37.422408,
    "base_lng": -122.084068,
    "distance": 5234.5,
    "bearing": 315.2,
    "heading": 0.0,
    "active_motor": 3,
    "gps_sats": 8
  }
}
```

Base station receives this and can display on dashboard!

## 📊 Web Dashboard Display

The navigation data is **automatically displayed on the web dashboard** when using the base station:

### Setup Dashboard Viewing:
1. Upload base station code: `pio run -e base_station -t upload`
2. Connect to WiFi: **"BeltStation"** (password: belt12345)
3. Open `frontend/index.html` in browser
4. Dashboard will automatically show navigation card when receiving data

### Dashboard Shows:
- **🧭 Distance to Base**: Remaining distance in meters or km
- **📍 Bearing to Base**: Absolute compass direction (0-360°)
- **🎯 Current Heading**: Your orientation from gyroscope
- **⚙️ Active Motor**: Which motor is vibrating (⬆️ FRONT, ➡️ RIGHT, ⬇️ BACK, ⬅️ LEFT)
- **📡 Base Station Coords**: Target location coordinates
- **🚦 Status Badge**: 
  - **"Navigating"** (>100m away)
  - **"Close"** (10-100m away, orange)
  - **"Arrived!"** (<10m away, green)

The navigation card appears automatically and has a **pulsing glow effect** to indicate active navigation!

## ⚠️ Important Notes

### GPS Fix Required
- GPS needs **clear view of sky**
- First fix takes **1-5 minutes**
- Indoor GPS will NOT work

### Heading Estimation
- Currently uses gyroscope integration (drift over time)
- For better accuracy, add magnetometer (compass)
- Or use GPS course over ground (requires movement)

### Motor Wiring
With L298N driver:
- **Front motors**: Connect to ENA, IN1, IN2
- **Back motors**: Connect to ENB, IN3, IN4
- **Left/Right**: Uses same drivers with reversed polarity

## 🐛 Troubleshooting

**No vibration:**
- Check GPS has fix (satellites > 4)
- Check motor connections
- Check motor power supply
- Verify base station coordinates are set

**Wrong motor vibrating:**
- Calibrate heading (magnetometer needed for accuracy)
- Check motor mounting orientation
- Verify base station coordinates

**Weak vibration:**
- Increase VIBRATION_STRENGTH
- Check motor power supply voltage
- Use higher voltage motors

## 🚀 Next Steps

To improve the system:
1. Add magnetometer (HMC5883L) for accurate heading
2. Add distance threshold (stop vibrating when at destination)
3. Add multiple waypoints (navigate through checkpoints)
4. Add haptic patterns (different patterns for different distances)
5. Add visual feedback (LED strip showing direction)

## 📚 Technical Details

**Bearing calculation**: Haversine formula
**Distance calculation**: Great circle distance
**Heading**: Gyroscope integration (can be improved with magnetometer)
**Motor control**: PWM at 5kHz, 8-bit resolution

---

**🎉 Now you have a wearable navigation system that guides you using touch!**
