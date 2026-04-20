// ============================================================
//  HAPTIC NAVIGATION BELT — ESP32
//  Sensors  : MPU6050 (accel + gyro) + HMC5883L (magnetometer)
//  Filter   : Tilt-compensated complementary filter
//  Nav      : Haversine bearing → relative angle → motor trigger
//
//  TEST MODE : Both base station and user coordinates are
//              hardcoded. Swap in live GPS later by replacing
//              USER_LAT / USER_LNG with GPS module output.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── GPS Pins ─────────────────────────────────────────────────
#define GPS_RX       16
#define GPS_TX       17
#define GPS_BAUD     9600
#define MIN_SPEED_KMH  0.8f   // GPS course trusted above this speed

// ── I2C Pins (ESP32 default) ─────────────────────────────────
#define SDA_PIN 21
#define SCL_PIN 22

// ── Vibration Motor GPIO Pins ─────────────────────────────────
// Wire each motor driver IN pin to these GPIOs
#define MOTOR_FRONT  25
#define MOTOR_RIGHT  26
#define MOTOR_BACK   27
#define MOTOR_LEFT   14

// ── Motor pulse duration (ms) ────────────────────────────────
// How long each vibration buzz lasts per loop cycle
#define MOTOR_PULSE_MS 200

// ── WiFi / Base Station ──────────────────────────────────────
#define WIFI_SSID            "BeltStation"
#define WIFI_PASSWORD        "belt12345"
#define BASE_STATION_URL     "http://192.168.4.1/api/data"
#define HTTP_POST_INTERVAL_MS  2000   // send telemetry every 2 s

// ── HMC5883L I2C Address & Registers ────────────────────────
#define MAG_ADDR      0x1E
#define MAG_REG_CFG_A 0x00
#define MAG_REG_CFG_B 0x01
#define MAG_REG_MODE  0x02
#define MAG_REG_DATA  0x03

// ── Magnetometer Hard-Iron Calibration Offsets ───────────────
// Set these after calibration (rotate sensor 360° and record
// min/max per axis, then offset = (max + min) / 2.0)
float MAG_OFFSET_X = 214.5f;
float MAG_OFFSET_Y = -228.0f;
float MAG_OFFSET_Z = 66.5f;

// ── Calibration Mode ─────────────────────────────────────────
// Set to true to run calibration: spin the belt slowly through
// a full 360° (tilt it in all directions too for best results).
// The live min/max and computed offsets print every second.
// Copy the "SET THESE" values into MAG_OFFSET_X/Y/Z above,
// then set CALIBRATION_MODE back to false and re-flash.
#define CALIBRATION_MODE false

// ── Complementary Filter Tuning ──────────────────────────────
// 0.99 = trust gyro almost entirely until mag is calibrated.
// Once MAG_OFFSET_X/Y/Z are filled in from calibration mode,
// lower this back to 0.90 to let the mag anchor long-term drift.
const float ALPHA = 0.90f;  // calibrated — mag can now contribute more

// ── Magnetometer Low-Pass Smoothing ──────────────────────────
// Blends current reading with previous: out = W*prev + (1-W)*new
const float MAG_SMOOTH_W = 0.80f;

// ── Gyro Dead-Zone ───────────────────────────────────────────
// Rotation rates below this threshold are treated as noise.
// 0.3 dps is safe: bias calibration is clean (corrected ≈0.00
// at rest), so anything above 0.3 is real rotation.
// Previous value of 1.0 was too high — slow turns were ignored.
const float GYRO_DEADZONE_DPS = 1.0f;

// ── Arrival Threshold ────────────────────────────────────────
// If user is within this many metres of base station,
// all motors buzz together to signal "you have arrived"
const float ARRIVAL_THRESHOLD_M = 3.0f;

// ════════════════════════════════════════════════════════════
//  TEST COORDINATES — CHANGE THESE TO YOUR REAL LOCATIONS
//
//  How to get coordinates:
//    Open Google Maps → long-press any point → copy lat,lng
//
//  BASE STATION : The fixed location the user must navigate to
//  USER         : Live GPS position read from NEO-M8N each loop
// ════════════════════════════════════════════════════════════
const double BASE_LAT =  6.927079;   // Example: Colombo, Sri Lanka
const double BASE_LNG = 79.861244;
// ════════════════════════════════════════════════════════════
 
// ── Global State ─────────────────────────────────────────────
Adafruit_MPU6050 mpu;

float heading_deg      = 0.0f;   // fused heading output (0-360 deg)
float prev_mag_heading = 0.0f;   // previous smoothed mag heading
float gyro_bias_z      = 0.0f;   // measured at startup, subtracted every loop
unsigned long last_ms  = 0;      // timestamp of last loop iteration

// ── GPS State ────────────────────────────────────────────────
TinyGPSPlus    gps;
HardwareSerial gpsSerial(2);

struct GpsData {
  double  latitude    = 0.0;
  double  longitude   = 0.0;
  float   altitude    = 0.0f;
  float   speed       = 0.0f;
  float   course      = 0.0f;
  uint8_t satellites  = 0;
  bool    locValid    = false;
  bool    courseValid = false;
} gpsData;

// ── Calibration min/max trackers ─────────────────────────────
int16_t cal_x_min =  32767, cal_x_max = -32768;
int16_t cal_y_min =  32767, cal_y_max = -32768;
int16_t cal_z_min =  32767, cal_z_max = -32768;


// ════════════════════════════════════════════════════════════
//  GPS HELPERS
// ════════════════════════════════════════════════════════════

void initGPS() {
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(100);
  Serial.println("[GPS]  NEO-M8N ready  (RX=16 TX=17 @ 9600)");
  Serial.println("       Waiting for satellite lock...");
}

void readGPS() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  gpsData.satellites = gps.satellites.value();

  if (gps.location.isValid()) {
    gpsData.latitude  = gps.location.lat();
    gpsData.longitude = gps.location.lng();
    gpsData.altitude  = gps.altitude.meters();
    gpsData.speed     = gps.speed.kmph();
    gpsData.locValid  = true;
  } else {
    gpsData.locValid  = false;
  }

  if (gps.course.isValid() && gpsData.speed >= MIN_SPEED_KMH) {
    gpsData.course      = gps.course.deg();
    gpsData.courseValid = true;
  } else {
    gpsData.courseValid = false;
  }
}

// ════════════════════════════════════════════════════════════
//  LOW-LEVEL I2C HELPERS FOR HMC5883L
// ════════════════════════════════════════════════════════════

/**
 * @brief  Write one byte to a magnetometer register.
 * @param  reg    Target register address.
 * @param  value  Byte to write.
 * @return true on ACK, false on NACK / bus error.
 */
bool magWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MAG_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return (Wire.endTransmission() == 0);
}

/**
 * @brief  Burst-read `len` bytes starting at `reg`.
 * @param  reg  First register to read from.
 * @param  buf  Destination buffer (must be at least `len` bytes).
 * @param  len  Number of bytes to read.
 * @return true on success, false on bus error or short read.
 */
bool magRead(uint8_t reg, uint8_t *buf, size_t len) {
  // Point the register pointer without releasing the bus
  Wire.beginTransmission(MAG_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  // Request the data bytes
  if (Wire.requestFrom((uint8_t)MAG_ADDR, (uint8_t)len) != len) return false;

  for (size_t i = 0; i < len; i++) {
    buf[i] = Wire.read();
  }
  return true;
}


// ════════════════════════════════════════════════════════════
//  HMC5883L INITIALISATION & RAW READ
// ════════════════════════════════════════════════════════════

/**
 * @brief  Initialise the HMC5883L magnetometer.
 *
 *  Config A  0x70 : 8-sample average, 15 Hz output rate, normal bias
 *  Config B  0xA0 : +/-4.7 Ga gain (adjust if readings saturate)
 *  Mode      0x00 : Continuous measurement mode
 *
 * @return true if all register writes succeeded.
 */
bool magInit() {
  if (!magWrite(MAG_REG_CFG_A, 0x70)) return false;  // 8-avg, 15 Hz
  if (!magWrite(MAG_REG_CFG_B, 0xA0)) return false;  // +/-4.7 Ga gain
  if (!magWrite(MAG_REG_MODE,  0x00)) return false;  // continuous mode
  return true;
}

/**
 * @brief  Read one magnetometer sample.
 *
 *  The HMC5883L data register layout is X_H, X_L, Z_H, Z_L, Y_H, Y_L
 *  (Z is in the middle — this is intentional by the manufacturer).
 *
 * @param  mx  Raw X axis (16-bit signed, big-endian).
 * @param  my  Raw Y axis.
 * @param  mz  Raw Z axis.
 * @return true on successful read.
 */
bool magReadRaw(int16_t &mx, int16_t &my, int16_t &mz) {
  uint8_t buf[6];
  if (!magRead(MAG_REG_DATA, buf, 6)) return false;

  // Reconstruct signed 16-bit values
  // Note: Z bytes are sandwiched between X and Y (HMC5883L quirk)
  mx = (int16_t)((buf[0] << 8) | buf[1]);
  mz = (int16_t)((buf[2] << 8) | buf[3]);
  my = (int16_t)((buf[4] << 8) | buf[5]);
  return true;
}


// ════════════════════════════════════════════════════════════
//  MATH HELPERS
// ════════════════════════════════════════════════════════════

/**
 * @brief  Wrap an angle into [0, 360).
 */
float normalize360(float angle) {
  while (angle <    0.0f) angle += 360.0f;
  while (angle >= 360.0f) angle -= 360.0f;
  return angle;
}

/**
 * @brief  Shortest signed angular difference: target - current in (-180, +180].
 *
 *  Used by the complementary filter so the correction always takes
 *  the short arc rather than spinning the long way around.
 */
float angleDiff(float target, float current) {
  float d = target - current;
  while (d >  180.0f) d -= 360.0f;
  while (d < -180.0f) d += 360.0f;
  return d;
}


// ════════════════════════════════════════════════════════════
//  HAVERSINE — BEARING & DISTANCE
// ════════════════════════════════════════════════════════════

/**
 * @brief  Compute the great-circle bearing from point A to point B.
 *
 *  The Haversine formula works on a spherical Earth model.
 *  Accuracy is ~0.3% which is more than enough for pedestrian nav.
 *
 *  Variable naming:
 *    lat1_r / lat2_r = latitudes converted to radians
 *    dLng_r          = longitude difference in radians
 *
 * @param  lat1, lng1  Origin  (user position)  in decimal degrees
 * @param  lat2, lng2  Target  (base station)   in decimal degrees
 * @return Bearing in degrees [0, 360),  0=North, 90=East
 */
float getBearing(double lat1, double lng1, double lat2, double lng2) {
  double lat1_r = lat1 * DEG_TO_RAD;
  double lat2_r = lat2 * DEG_TO_RAD;
  double dLng_r = (lng2 - lng1) * DEG_TO_RAD;

  double x = sin(dLng_r) * cos(lat2_r);
  double y = cos(lat1_r) * sin(lat2_r) - sin(lat1_r) * cos(lat2_r) * cos(dLng_r);

  float bearing = atan2(x, y) * RAD_TO_DEG;
  return normalize360(bearing);
}

/**
 * @brief  Compute straight-line distance between two GPS points in metres.
 *
 *  Variable naming:
 *    lat1_r / lat2_r = latitudes in radians
 *    dLat_r / dLng_r = lat/lng deltas in radians
 *
 * @param  lat1, lng1  Point A in decimal degrees
 * @param  lat2, lng2  Point B in decimal degrees
 * @return Distance in metres
 */
float getDistance(double lat1, double lng1, double lat2, double lng2) {
  const float R = 6371000.0f;  // Earth radius in metres

  double lat1_r = lat1 * DEG_TO_RAD;
  double lat2_r = lat2 * DEG_TO_RAD;
  double dLat_r = (lat2 - lat1) * DEG_TO_RAD;
  double dLng_r = (lng2 - lng1) * DEG_TO_RAD;

  double a = sin(dLat_r/2) * sin(dLat_r/2) +
             cos(lat1_r) * cos(lat2_r) * sin(dLng_r/2) * sin(dLng_r/2);

  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return (float)(R * c);
}


// ════════════════════════════════════════════════════════════
//  MOTOR CONTROL
// ════════════════════════════════════════════════════════════

/**
 * @brief  Turn all motors off.
 *  Always called before triggering a new motor to prevent
 *  multiple motors running simultaneously.
 */
void motorsOff() {
  digitalWrite(MOTOR_FRONT, LOW);
  digitalWrite(MOTOR_BACK,  LOW);
  digitalWrite(MOTOR_LEFT,  LOW);
  digitalWrite(MOTOR_RIGHT, LOW);
}

/**
 * @brief  Pulse all four motors simultaneously.
 *  Used to signal arrival at the base station.
 */
void buzzArrival() {
  digitalWrite(MOTOR_FRONT, HIGH);
  digitalWrite(MOTOR_BACK,  HIGH);
  digitalWrite(MOTOR_LEFT,  HIGH);
  digitalWrite(MOTOR_RIGHT, HIGH);
  delay(MOTOR_PULSE_MS);
  motorsOff();
}

/**
 * @brief  Fire the correct motor based on where the base station
 *         is relative to the direction the belt is facing.
 *
 *  rel_angle is the angle from the belt's forward axis to the
 *  base station, measured clockwise:
 *    Positive  = base station is to the RIGHT
 *    Negative  = base station is to the LEFT
 *    Near zero = base station is straight ahead
 *
 *  Motor sectors (each covers a 90 degree window):
 *
 *              FRONT  (-45 to +45 deg)
 *                     UP
 *    LEFT             |            RIGHT
 *  (-135 to -45 deg)  |   (+45 to +135 deg)
 *                     |
 *              BACK  (+-135 to +-180 deg)
 *
 * @param  rel_angle  Range: (-180, +180]
 */
void triggerMotor(float rel_angle) {
  motorsOff();  // clear previous state first

  if (rel_angle >= -45.0f && rel_angle <= 45.0f) {
    // Base station is roughly ahead — keep moving forward
    Serial.println("  [FRONT]  Keep going — base station is ahead");
    digitalWrite(MOTOR_FRONT, HIGH);

  } else if (rel_angle > 45.0f && rel_angle <= 135.0f) {
    // Base station is to the right — rotate clockwise
    Serial.println("  [RIGHT]  Rotate right to face base station");
    digitalWrite(MOTOR_RIGHT, HIGH);

  } else if (rel_angle < -45.0f && rel_angle >= -135.0f) {
    // Base station is to the left — rotate counter-clockwise
    Serial.println("  [LEFT]   Rotate left to face base station");
    digitalWrite(MOTOR_LEFT, HIGH);

  } else {
    // Base station is behind — turn around
    Serial.println("  [BACK]   Turn around — base station is behind you");
    digitalWrite(MOTOR_BACK, HIGH);
  }

  delay(MOTOR_PULSE_MS);
  motorsOff();
}


// ════════════════════════════════════════════════════════════
//  WIFI / HTTP HELPERS
// ════════════════════════════════════════════════════════════

/**
 * @brief  Map relative angle + distance to the nav state string
 *         expected by the base station dashboard.
 */
const char* navStateStr(float rel_angle, float dist) {
  if (dist <= ARRIVAL_THRESHOLD_M)                return "ARRIVED";
  if (rel_angle >= -45.0f && rel_angle <=  45.0f) return "FORWARD";
  if (rel_angle >  45.0f  && rel_angle <= 135.0f) return "TURN_RIGHT";
  if (rel_angle < -45.0f  && rel_angle >= -135.0f) return "TURN_LEFT";
  return "TURN_BACK";
}

/**
 * @brief  Connect to the base station WiFi AP.
 *         Blocks up to 10 s on first call; retried on every failed POST.
 */
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed — will retry on next POST");
  }
}

static unsigned long last_post_ms = 0;

/**
 * @brief  Serialise all telemetry to JSON and HTTP POST to the base station.
 *         Rate-limited to HTTP_POST_INTERVAL_MS.  Reconnects WiFi if dropped.
 */
void postTelemetry(float ax, float ay, float az,
                   float gx, float gy, float gz,
                   float temp_c,
                   float heading, float bearing, float rel_angle, float dist) {
  if (millis() - last_post_ms < HTTP_POST_INTERVAL_MS) return;
  last_post_ms = millis();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  StaticJsonDocument<512> doc;

  JsonObject gpsObj = doc.createNestedObject("gps");
  gpsObj["valid"]      = gpsData.locValid;
  gpsObj["lat"]        = (double)gpsData.latitude;
  gpsObj["lng"]        = (double)gpsData.longitude;
  gpsObj["alt"]        = gpsData.altitude;
  gpsObj["speed"]      = gpsData.speed;
  gpsObj["satellites"] = gpsData.satellites;

  JsonObject imuObj = doc.createNestedObject("imu");
  imuObj["valid"]   = true;
  imuObj["accel_x"] = ax;
  imuObj["accel_y"] = ay;
  imuObj["accel_z"] = az;
  imuObj["gyro_x"]  = gx;
  imuObj["gyro_y"]  = gy;
  imuObj["gyro_z"]  = gz;
  imuObj["temp"]    = temp_c;

  JsonObject navObj = doc.createNestedObject("nav");
  navObj["fused_heading"] = heading;
  navObj["bearing"]       = bearing;
  navObj["rel_bearing"]   = rel_angle;
  navObj["distance"]      = dist;
  navObj["state"]         = navStateStr(rel_angle, dist);
  navObj["bias_z"]        = gyro_bias_z;
  navObj["heading_src"]   = "GYRO";

  JsonObject hrObj = doc.createNestedObject("hr");
  hrObj["valid"]  = false;
  hrObj["finger"] = false;
  hrObj["bpm"]    = 0;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(BASE_STATION_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("[HTTP] POST %d  (%d bytes)\n", code, (int)body.length());
  } else {
    Serial.printf("[HTTP] POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}


// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // ── GPS ────────────────────────────────────────────────────
  initGPS();

  // ── Motor pins ─────────────────────────────────────────────
  pinMode(MOTOR_FRONT, OUTPUT);
  pinMode(MOTOR_BACK,  OUTPUT);
  pinMode(MOTOR_LEFT,  OUTPUT);
  pinMode(MOTOR_RIGHT, OUTPUT);
  motorsOff();

  // ── WiFi → Base Station ─────────────────────────────────────
  connectWiFi();

  // ── I2C bus ────────────────────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);

  // ── MPU6050 ────────────────────────────────────────────────
  if (!mpu.begin()) {
    Serial.println("[ERROR] MPU6050 not found — check wiring");
    while (1) delay(500);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_4_G);    // +/-4g, enough for tilt
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);         // +/-500 deg/s
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);      // DLPF cuts vibration noise
  Serial.println("[OK] MPU6050 ready");

  // ── HMC5883L ───────────────────────────────────────────────
  if (!magInit()) {
    Serial.println("[ERROR] HMC5883L not found — check wiring");
    while (1) delay(500);
  }
  Serial.println("[OK] HMC5883L ready");

  // ── Print startup nav info (printed once GPS gets fix) ────
  Serial.println("────────────────────────────────────────");
  Serial.printf ("  Base station : %.6f, %.6f\n", BASE_LAT, BASE_LNG);
  Serial.println("  User position: live GPS (waiting for fix...)");
  Serial.println("────────────────────────────────────────");

  // Gyro bias calibration — keep sensor STILL during this!
  Serial.println("Calibrating gyro — keep sensor still...");
  float bias_sum = 0.0f;
  const int CALIB_SAMPLES = 100;
  for (int i = 0; i < CALIB_SAMPLES; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    bias_sum += g.gyro.z * RAD_TO_DEG;  // accumulate raw deg/s readings
    delay(10);
  }
  gyro_bias_z = bias_sum / CALIB_SAMPLES;  // average = resting bias
  Serial.print("Gyro Z bias: ");
  Serial.print(gyro_bias_z, 4);
  Serial.println(" deg/s");

  // ── Seed heading from magnetometer ─────────────────────────
  // heading_deg defaults to 0.  While stationary the dead-zone
  // gate prevents any correction, so we must prime it now from
  // a few averaged mag samples so the belt starts with the real
  // compass heading instead of zero.
  {
    float mag_seed_sum = 0.0f;
    int   mag_seed_ok  = 0;
    const int SEED_SAMPLES = 10;
    for (int i = 0; i < SEED_SAMPLES; i++) {
      int16_t sx, sy, sz;
      if (magReadRaw(sx, sy, sz)) {
        sensors_event_t a, g, t;
        mpu.getEvent(&a, &g, &t);
        float ax_ = a.acceleration.x;
        float ay_ = a.acceleration.y;
        float az_ = a.acceleration.z;

        float fx = (sx - MAG_OFFSET_X);
        float fy = (sy - MAG_OFFSET_Y);
        float fz = (sz - MAG_OFFSET_Z);

        float roll_  = atan2f(ay_, az_);
        float pitch_ = atan2f(-ax_, sqrtf(ay_*ay_ + az_*az_));

        float mx_h = fx * cosf(pitch_) + fz * sinf(pitch_);
        float my_h = fx * sinf(roll_) * sinf(pitch_)
                   + fy * cosf(roll_)
                   - fz * sinf(roll_) * cosf(pitch_);

        float h = normalize360(atan2f(my_h, mx_h) * RAD_TO_DEG);
        mag_seed_sum += h;
        mag_seed_ok++;
      }
      delay(20);
    }
    if (mag_seed_ok > 0) {
      heading_deg      = mag_seed_sum / mag_seed_ok;
      prev_mag_heading = heading_deg;
      Serial.print("[OK] Heading seeded from magnetometer: ");
      Serial.print(heading_deg, 1);
      Serial.println(" deg");
    } else {
      Serial.println("[WARN] Mag seed failed — heading starts at 0");
    }
  }

  last_ms = millis();
}


// ════════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════════

void loop() {

  // ── 0. Read GPS ────────────────────────────────────────────
  readGPS();

  // ── 1. Read IMU ────────────────────────────────────────────
  sensors_event_t accel_evt, gyro_evt, temp_evt;
  mpu.getEvent(&accel_evt, &gyro_evt, &temp_evt);

  float ax = accel_evt.acceleration.x;   // m/s^2
  float ay = accel_evt.acceleration.y;
  float az = accel_evt.acceleration.z;

  // Gyroscope yaw rate: convert rad/s to deg/s
  float gyro_z_dps = gyro_evt.gyro.z * RAD_TO_DEG;
  float gyro_x_dps = gyro_evt.gyro.x * RAD_TO_DEG;
  float gyro_y_dps = gyro_evt.gyro.y * RAD_TO_DEG;
  float imu_temp   = temp_evt.temperature;


  // ── 2. Read raw magnetometer ───────────────────────────────
  // If the HMC5883L drops a sample, flag it but do NOT return.
  // Navigation still runs using the last known heading_deg — motor
  // pulses keep firing uninterrupted. Only the fusion update is skipped.
  int16_t mx_raw, my_raw, mz_raw;
  bool magOk = magReadRaw(mx_raw, my_raw, mz_raw);
  if (!magOk) {
    Serial.println("[WARN] Magnetometer read failed — using last heading");
  }


  // ── 3. Delta time ──────────────────────────────────────────
  unsigned long now = millis();
  float dt = (now - last_ms) / 1000.0f;   // convert ms to seconds
  last_ms  = now;
  if (dt > 0.5f) dt = 0.5f;   // clamp: prevents huge jump on first loop


  if (magOk) {
    // ── 4. Apply hard-iron calibration offsets ───────────────
    float mx = mx_raw - MAG_OFFSET_X;
    float my = my_raw - MAG_OFFSET_Y;
    float mz = mz_raw - MAG_OFFSET_Z;

    // ── 5. Tilt compensation ─────────────────────────────────
    // Roll and pitch from accelerometer rotate the mag vector
    // back into the horizontal plane before computing heading.
    float roll  = atan2f(ay, az);
    float pitch = atan2f(-ax, sqrtf(ay*ay + az*az));

    float mx_h = mx * cosf(pitch)
               + mz * sinf(pitch);

    float my_h = mx * sinf(roll) * sinf(pitch)
               + my * cosf(roll)
               - mz * sinf(roll) * cosf(pitch);

    float mag_heading = normalize360(atan2f(my_h, mx_h) * RAD_TO_DEG);

    // ── 6. Low-pass smooth the magnetometer heading ──────────
    mag_heading      = MAG_SMOOTH_W * prev_mag_heading
                     + (1.0f - MAG_SMOOTH_W) * mag_heading;
    prev_mag_heading = mag_heading;

    // ── 7. Gyro dead-zone ────────────────────────────────────
    // Subtract calibrated bias; ignore sub-threshold noise.
    float gyro_z_corrected = gyro_z_dps - gyro_bias_z;
    if (fabsf(gyro_z_corrected) < GYRO_DEADZONE_DPS) {
      gyro_z_corrected = 0.0f;
    }

    // ── 8. Complementary filter ──────────────────────────────
    // Always runs — two modes:
    //
    // ROTATING (gyro above dead-zone):
    //   Full complementary blend: 90% gyro step + 10% mag correction.
    //   Gyro tracks fast turns smoothly, mag prevents long-term drift.
    //
    // STATIONARY (gyro in dead-zone):
    //   Gentle mag-only correction: 2% pull per cycle toward compass.
    //   This ensures the heading converges to the real compass direction
    //   even when the user is standing still holding a new orientation.
    //   Without this, heading_deg would freeze forever after every turn.
    if (gyro_z_corrected != 0.0f) {
      // Rotating: full complementary filter
      float gyro_heading = normalize360(heading_deg + gyro_z_corrected * dt);
      float correction   = angleDiff(mag_heading, gyro_heading);
      heading_deg        = normalize360(gyro_heading + (1.0f - ALPHA) * correction);
    } else {
      // Stationary: gentle mag pull only (no gyro step)
      float correction = angleDiff(mag_heading, heading_deg);
      heading_deg      = normalize360(heading_deg + 0.05f * correction);
    }
  }
  // If magOk == false: skip steps 4-8 entirely, heading_deg is unchanged.


  // ── 9. Navigation calculation ─────────────────────────────
  // Use live GPS position when available; hold last known values
  // when no fix so motors don't cut out mid-navigation.
  static float target_bearing = 0.0f;
  static float distance_m     = 0.0f;

  if (gpsData.locValid) {
    target_bearing = getBearing (gpsData.latitude, gpsData.longitude, BASE_LAT, BASE_LNG);
    distance_m     = getDistance(gpsData.latitude, gpsData.longitude, BASE_LAT, BASE_LNG);
  }

  // Relative angle = how far and in which direction the user
  // needs to rotate to face the base station
  //   Positive = base station is to the RIGHT
  //   Negative = base station is to the LEFT
  //   Near 0   = base station is straight ahead
  float relative_angle = angleDiff(target_bearing, heading_deg);


  // ── 10. Motor trigger ──────────────────────────────────────
  if (distance_m <= ARRIVAL_THRESHOLD_M) {
    // Within arrival threshold — buzz all motors as confirmation
    Serial.println("  *** ARRIVED at base station ***");
    buzzArrival();
  } else {
    // Guide the user toward the base station
    triggerMotor(relative_angle);
  }

  // ── HTTP Telemetry → Base Station ─────────────────────────
  postTelemetry(ax, ay, az,
                gyro_x_dps, gyro_y_dps, gyro_z_dps,
                imu_temp,
                heading_deg, target_bearing, relative_angle, distance_m);


  // ── 11. Debug serial output ────────────────────────────────
  Serial.println("════════════════════════════════════════");
  Serial.print  ("Belt heading  : "); Serial.print(heading_deg,    1); Serial.println(" deg");
  Serial.print  ("Target bearing: "); Serial.print(target_bearing, 1); Serial.println(" deg");
  Serial.print  ("Relative angle: "); Serial.print(relative_angle, 1); Serial.println(" deg");
  Serial.print  ("Distance      : "); Serial.print(distance_m,     1); Serial.println(" m");
  Serial.println("────────────────────────────────────────");
  // GPS
  if (gpsData.locValid) {
    Serial.printf("GPS    Lat=%.6f  Lng=%.6f  Alt=%.1fm  Spd=%.1fkm/h  Sats=%d\n",
                  gpsData.latitude, gpsData.longitude,
                  gpsData.altitude, gpsData.speed, gpsData.satellites);
    if (gpsData.courseValid)
      Serial.printf("       Course: %.1f deg  (moving)\n", gpsData.course);
    else
      Serial.printf("       Course: --  (speed below %.1f km/h)\n", MIN_SPEED_KMH);
  } else {
    Serial.printf("GPS    NO FIX  Sats: %d\n", gpsData.satellites);
  }
  Serial.println("────────────────────────────────────────");
  // IMU raw data
  Serial.printf ("Accel  X=%6.2f  Y=%6.2f  Z=%6.2f  m/s²\n",
                 ax, ay, az);
  Serial.printf ("Gyro   Z=%6.2f  (bias=%.4f)  corrected=%6.2f  deg/s\n",
                 gyro_z_dps, gyro_bias_z, gyro_z_dps - gyro_bias_z);
  // Magnetometer (last valid reading, or raw if just read)
  if (magOk) {
    Serial.printf("Mag    X=%-6d  Y=%-6d  Z=%-6d  (raw)\n",
                  (int)mx_raw, (int)my_raw, (int)mz_raw);
    Serial.printf("Mag heading (smoothed) : %.1f deg\n", prev_mag_heading);
  } else {
    Serial.println("Mag    [no sample — last heading held]");
  }
  Serial.println("════════════════════════════════════════");


  // ── Calibration mode ───────────────────────────────────────
  // When CALIBRATION_MODE true: skip nav, just print live
  // min/max and the computed offsets. Spin the belt 360° slowly.
#if CALIBRATION_MODE
  if (magOk) {
    if (mx_raw < cal_x_min) cal_x_min = mx_raw;
    if (mx_raw > cal_x_max) cal_x_max = mx_raw;
    if (my_raw < cal_y_min) cal_y_min = my_raw;
    if (my_raw > cal_y_max) cal_y_max = my_raw;
    if (mz_raw < cal_z_min) cal_z_min = mz_raw;
    if (mz_raw > cal_z_max) cal_z_max = mz_raw;

    Serial.println("──── CALIBRATION MODE ────────────────────");
    Serial.printf("Raw   X=%-6d  Y=%-6d  Z=%-6d\n", mx_raw, my_raw, mz_raw);
    Serial.printf("Min   X=%-6d  Y=%-6d  Z=%-6d\n", cal_x_min, cal_y_min, cal_z_min);
    Serial.printf("Max   X=%-6d  Y=%-6d  Z=%-6d\n", cal_x_max, cal_y_max, cal_z_max);
    Serial.printf("SET THESE >>  OFFSET_X=%.1f  OFFSET_Y=%.1f  OFFSET_Z=%.1f\n",
                  (cal_x_min + cal_x_max) / 2.0f,
                  (cal_y_min + cal_y_max) / 2.0f,
                  (cal_z_min + cal_z_max) / 2.0f);
    Serial.println("──────────────────────────────────────────");
  }
#endif

  delay(100);   // 10 Hz update rate
}