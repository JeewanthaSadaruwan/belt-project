/*
 * ============================================================
 *  NAVIGATION BELT v3.0
 *  GPS + Gyro Dead-Reckoning Heading Fusion
 * ============================================================
 *
 *  HARDWARE:
 *    GPS NEO-M8N    UART2   RX=16, TX=17
 *    MPU6050 IMU    I2C #1  SDA=21, SCL=22   (Wire)
 *    MAX30102       I2C #2  SDA=32, SCL=33   (Wire1)
 *    Status LED             GPIO 2
 *    Motor FRONT    L298N#1 GPIO 25
 *    Motor RIGHT    L298N#1 GPIO 26
 *    Motor BACK     L298N#2 GPIO 27
 *    Motor LEFT     L298N#2 GPIO 14
 *
 *  HEADING FUSION STRATEGY:
 *    The MPU6050 has no magnetometer so it cannot give an
 *    absolute compass heading. Instead we fuse two sources:
 *
 *    1. GPS Course-over-Ground (COG)
 *       - Only valid when speed >= MIN_SPEED_KMH (0.8 km/h)
 *       - When valid -> LOCK fusedHeading to GPS course
 *         and reset gyro drift accumulator
 *
 *    2. Gyro Z-axis Dead-Reckoning
 *       - When stationary / below speed threshold:
 *         integrate angular velocity (rad/s -> deg) over time
 *         to track how much the wearer has rotated since
 *         the last GPS lock
 *       - Bias (zero-rate offset) is calibrated at startup
 *         by averaging 200 stationary samples
 *
 *    3. Staleness Flag
 *       - If no GPS re-lock for > GYRO_STALE_MS (20s):
 *         heading is flagged STALE -- gyro drift has likely
 *         accumulated too much to trust
 *       - Navigation continues with best-guess heading
 *         but serial output shows [STALE] warning
 *
 *  NAVIGATION LOGIC:
 *    relativeBearing = normAngle(bearingToBase - fusedHeading)
 *
 *    Zone   Relative Bearing      Motor   Pattern
 *    ---------------------------------------------------------
 *    FRONT  -45 to +45 deg        FRONT   Pulse 250/250 ms
 *                                           "On track, go!"
 *    RIGHT  +45 to +135 deg       RIGHT   Steady
 *                                           "Turn right"
 *    BACK   +-135 to +-180 deg    BACK    Steady
 *                                           "Turn around"
 *    LEFT   -45 to -135 deg       LEFT    Steady
 *                                           "Turn left"
 *    ---------------------------------------------------------
 *    ARRIVED  dist <= 5m          ALL     Fast pulse 150/150ms
 *    NO GPS                       --      All off
 *    INIT (no GPS lock yet)       FRONT   Slow pulse 600/900ms
 *                                           "Walk to calibrate"
 *
 *  BASE STATION:
 *    Colombo Fort Railway Station, Sri Lanka
 *    Lat: 6.934200   Lng: 79.850600   (~25 km from Chilaw coast)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_HMC5883_U.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <math.h>

// ============================================================
//  BASE STATION COORDINATES
// ============================================================
// Colombo Fort Railway Station — ~25 km from Chilaw coast
#define BASE_LAT              6.934200
#define BASE_LNG              79.850600
#define ARRIVED_RADIUS_M      15.0   // Enter ARRIVED when dist < 15m
#define ARRIVED_EXIT_M        30.0   // Exit ARRIVED only when dist > 30m (hysteresis)
#define MIN_SATS_FOR_NAV      5      // Need at least 5 sats for reliable position

// ============================================================
//  WIFI
// ============================================================
#define WIFI_SSID             "BeltStation"
#define WIFI_PASSWORD         "belt12345"
#define BASE_URL              "http://192.168.4.1/api/data"

// ============================================================
//  PINS
// ============================================================
#define GPS_RX                16
#define GPS_TX                17
#define GPS_BAUD              9600
#define IMU_SDA               21
#define IMU_SCL               22
#define MAX_SDA               32   // MAX30102 on Wire1
#define MAX_SCL               33
#define STATUS_LED            2
#define MOTOR_FRONT           25
#define MOTOR_RIGHT           26
#define MOTOR_BACK            27
#define MOTOR_LEFT            14

// ============================================================
//  TIMING
// ============================================================
#define SEND_INTERVAL_MS      2000
#define NAV_UPDATE_MS         100    // 10 Hz nav loop
#define WIFI_RETRY_MS         5000

// Vibration pulse timings (ms)
#define PULSE_FORWARD_ON      250    // FRONT pulse: on time
#define PULSE_FORWARD_OFF     250    // FRONT pulse: off time
#define PULSE_ARRIVED_ON      150    // ARRIVED pulse: on time
#define PULSE_ARRIVED_OFF     150    // ARRIVED pulse: off time

// ============================================================
//  HEADING FUSION PARAMETERS
// ============================================================
#define MIN_SPEED_KMH         0.8f   // GPS course trusted above this
#define GYRO_BIAS_SAMPLES     200    // Samples for bias calibration
#define GYRO_STALE_MS         20000  // Gyro-only trusted for 20s max

// Set to -1.0 if rotation direction is inverted on your board.
// Positive = clockwise rotation increases heading (standard).
#define GYRO_Z_AXIS_SIGN      1.0f

// ── Complementary Filter (HMC5983 + Gyro) ──────────────────
#define COMP_ALPHA            0.98f  // Gyro weight per cycle (0.98 = 98% gyro, 2% mag)
#define MAG_SMOOTH_W          0.80f  // Mag low-pass weight (higher = smoother but slower)

// Hard-iron calibration offsets — run a calibration sketch and fill these in.
// Spin the belt 360° and record min/max on each axis:
//   OFFSET = (max + min) / 2
#define MAG_OFFSET_X          214.5f
#define MAG_OFFSET_Y          -228.0f
#define MAG_OFFSET_Z          66.5f

// Navigation zones
#define FRONT_ZONE_DEG        45.0f
#define SIDE_ZONE_DEG         135.0f

// ============================================================
//  OBJECTS
// ============================================================
TinyGPSPlus         gps;
HardwareSerial      gpsSerial(2);
Adafruit_MPU6050    mpu;
Adafruit_HMC5883_U  mag(12345);  // HMC5983 — same I2C bus (SDA=21, SCL=22) addr 0x1E
MAX30105            particleSensor;

// Heart rate beat detection state
#define HR_SAMPLE_SIZE   4          // Rolling average over 4 beat intervals
byte    hrRates[HR_SAMPLE_SIZE];    // Circular buffer of BPM readings
byte    hrRateIdx    = 0;
long    hrLastBeat   = 0;           // millis() of last detected beat
float   hrBPM        = 0.0f;        // Latest instantaneous BPM
float   hrAvgBPM     = 0.0f;        // Rolling average BPM

// ============================================================
//  SENSOR DATA
// ============================================================
struct SensorData {
  // GPS
  double    latitude    = 0.0;
  double    longitude   = 0.0;
  float     altitude    = 0.0f;
  float     speed       = 0.0f;
  float     gpsCourse   = 0.0f;
  uint8_t   satellites  = 0;
  bool      gpsValid    = false;
  bool      courseValid = false;

  // IMU (raw)
  float     accelX      = 0.0f;
  float     accelY      = 0.0f;
  float     accelZ      = 0.0f;
  float     gyroX       = 0.0f;
  float     gyroY       = 0.0f;
  float     gyroZ       = 0.0f;
  float     temperature = 0.0f;
  bool      imuValid    = false;

  // HMC5983 Magnetometer
  bool      magValid    = false;

  // MAX30102 Pulse Oximeter
  float     heartRate   = 0.0f;     // BPM (rolling average)
  float     irValue     = 0.0f;     // Raw IR reading (finger detect)
  bool      fingerOn    = false;    // True when finger is placed
  bool      maxValid    = false;
};

SensorData sensorData;

// ============================================================
//  HEADING FUSION STATE
// ============================================================
enum HeadingSource {
  HEADING_NONE,     // Boot: gyro only, no absolute reference yet
  HEADING_GPS,      // Snapped to GPS COG (moving)
  HEADING_COMP,     // Complementary filter: HMC5983 mag + gyro fused
  HEADING_GYRO,     // Gyro dead-reckoning (mag unavailable)
  HEADING_STALE     // Gyro only, no GPS or mag correction for > 20s
};

float         fusedHeading       = 0.0f;   // Starts at 0° (boot-relative North)
float         gyroBiasZ          = 0.0f;
float         magHeadingSmoothed = 0.0f;   // Low-pass filtered magnetometer heading
bool          headingInitialised = true;   // Gyro integrates from boot.
                                           // GPS lock later snaps to true North.
HeadingSource headingSource      = HEADING_GYRO; // Gyro-relative from startup
unsigned long lastGpsLockMs      = 0;
unsigned long lastGyroUpdateMs   = 0;

// ============================================================
//  NAVIGATION OUTPUT
// ============================================================
enum NavState {
  NAV_NO_GPS,
  NAV_INIT,
  NAV_ALIGN_FRONT,
  NAV_TURN_RIGHT,
  NAV_TURN_BACK,
  NAV_TURN_LEFT,
  NAV_ARRIVED
};

NavState      navState        = NAV_NO_GPS;
NavState      prevNavState    = NAV_NO_GPS;  // detect state changes
float         bearingToBase   = 0.0f;
float         distanceToBase  = 0.0f;
float         relativeBearing = 0.0f;
bool          isArrived       = false;  // hysteresis flag: stays true until EXIT threshold

// Separate pulse timers per state — avoids cross-state interference
unsigned long pulseFrontTimer   = 0;
bool          pulseFrontOn      = false;
unsigned long pulseArrivedTimer = 0;
bool          pulseArrivedOn    = false;

// Timers
unsigned long lastSend        = 0;
unsigned long lastNav         = 0;
unsigned long lastWifiAttempt = 0;
int           packetsSent     = 0;

const int MOTORS[4] = {MOTOR_FRONT, MOTOR_RIGHT, MOTOR_BACK, MOTOR_LEFT};

// ============================================================
//  MOTOR HELPERS
// ============================================================
void allMotorsOff() {
  for (int i = 0; i < 4; i++) digitalWrite(MOTORS[i], LOW);
}

// ============================================================
//  MATH
// ============================================================
float calcBearing(double lat1, double lng1, double lat2, double lng2) {
  double dLng  = (lng2 - lng1) * DEG_TO_RAD;
  double rLat1 = lat1 * DEG_TO_RAD;
  double rLat2 = lat2 * DEG_TO_RAD;
  double y     = sin(dLng) * cos(rLat2);
  double x     = cos(rLat1)*sin(rLat2) - sin(rLat1)*cos(rLat2)*cos(dLng);
  return fmod((float)(atan2(y, x) * RAD_TO_DEG) + 360.0f, 360.0f);
}

float calcDistance(double lat1, double lng1, double lat2, double lng2) {
  const double R = 6371000.0;
  double dLat = (lat2 - lat1) * DEG_TO_RAD;
  double dLng = (lng2 - lng1) * DEG_TO_RAD;
  double a    = sin(dLat/2)*sin(dLat/2)
              + cos(lat1*DEG_TO_RAD)*cos(lat2*DEG_TO_RAD)
                *sin(dLng/2)*sin(dLng/2);
  return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

float normAngle(float a) {
  while (a >  180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

float wrapHeading(float h) {
  h = fmod(h, 360.0f);
  if (h < 0) h += 360.0f;
  return h;
}

// Returns shortest signed arc from current to target: range (-180, +180]
float angleDiff(float target, float current) {
  float d = target - current;
  while (d >  180.0f) d -= 360.0f;
  while (d < -180.0f) d += 360.0f;
  return d;
}

// ── Tilt-compensated magnetometer heading ─────────────────────────
// Uses accelerometer roll/pitch to project the magnetic vector onto
// the horizontal plane before computing heading. Without this, any
// belt tilt introduces heading error proportional to tilt angle.
float readMagHeading() {
  sensors_event_t event;
  mag.getEvent(&event);

  // Apply hard-iron calibration offsets
  float mx = event.magnetic.x - MAG_OFFSET_X;
  float my = event.magnetic.y - MAG_OFFSET_Y;
  float mz = event.magnetic.z - MAG_OFFSET_Z;

  // Roll and pitch from accelerometer (gravity as reference)
  float ax = sensorData.accelX;
  float ay = sensorData.accelY;
  float az = sensorData.accelZ;
  float roll  = atan2f(ay, az);
  float pitch = atan2f(-ax, sqrtf(ay*ay + az*az));

  // Project magnetic vector onto horizontal plane
  float mx_h = mx * cosf(pitch) + mz * sinf(pitch);
  float my_h = mx * sinf(roll) * sinf(pitch)
             + my * cosf(roll)
             - mz * sinf(roll) * cosf(pitch);

  return wrapHeading(atan2f(my_h, mx_h) * RAD_TO_DEG);
}

// ============================================================
//  PULSE HELPERS — one per state, no shared timer interference
// ============================================================

// FRONT pulse: short snappy on/off = "aligned, keep going"
bool tickFrontPulse() {
  unsigned long now    = millis();
  unsigned long period = pulseFrontOn ? PULSE_FORWARD_ON : PULSE_FORWARD_OFF;
  if (now - pulseFrontTimer >= period) {
    pulseFrontOn    = !pulseFrontOn;
    pulseFrontTimer = now;
  }
  return pulseFrontOn;
}

// ARRIVED pulse: fast all-motor burst = "you're here!"
bool tickArrivedPulse() {
  unsigned long now    = millis();
  unsigned long period = pulseArrivedOn ? PULSE_ARRIVED_ON : PULSE_ARRIVED_OFF;
  if (now - pulseArrivedTimer >= period) {
    pulseArrivedOn    = !pulseArrivedOn;
    pulseArrivedTimer = now;
  }
  return pulseArrivedOn;
}

// ============================================================
//  HEADING FUSION
// ============================================================

/*
 * Calibrate gyro Z bias at startup.
 * Device must be still. Averages GYRO_BIAS_SAMPLES readings.
 */
void calibrateGyroBias() {
  Serial.println("[GYRO] Calibrating bias -- keep device still for 1 second...");
  double sum = 0.0;
  for (int i = 0; i < GYRO_BIAS_SAMPLES; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    sum += g.gyro.z;
    delay(5);
  }
  gyroBiasZ = (float)(sum / GYRO_BIAS_SAMPLES);
  Serial.printf("[GYRO] Bias Z = %.5f rad/s  (%.2f deg/s)\n",
    gyroBiasZ, gyroBiasZ * RAD_TO_DEG);
}

/*
 * Snap fused heading to GPS truth and re-sync mag smoother.
 * Called whenever GPS COG is valid and speed is sufficient.
 */
void lockHeadingFromGPS(float gpsCourse) {
  fusedHeading        = gpsCourse;
  magHeadingSmoothed  = gpsCourse;   // re-sync mag smoother to GPS truth
  headingInitialised  = true;
  headingSource       = HEADING_GPS;
  lastGpsLockMs       = millis();
  lastGyroUpdateMs    = millis();
}

/*
 * Complementary filter — HMC5983 magnetometer + MPU6050 gyro.
 *
 * Every cycle:
 *   1. Gyro step:   gyroHeading = fusedHeading + (gyroZ - bias) * dt
 *   2. Mag read:    tilt-compensated compass heading
 *   3. Mag smooth:  low-pass filter to suppress spiky readings
 *   4. Blend:       fusedHeading = gyroHeading + (1 - ALPHA) * angleDiff(mag, gyro)
 *                   ALPHA = 0.98 → 98% gyro trust, 2% mag correction per cycle
 *
 * Benefits over pure dead-reckoning:
 *   - Mag prevents long-term drift (works when stationary)
 *   - Gyro prevents mag noise from causing jitter
 *   - Works even with no GPS signal
 */
void updateComplementaryFilter() {
  if (!sensorData.imuValid) return;

  unsigned long now = millis();
  float dt = (now - lastGyroUpdateMs) / 1000.0f;
  lastGyroUpdateMs = now;

  if (dt <= 0.0f || dt > 1.0f) return;

  // ── Step 1: Gyro integration ────────────────────────────
  float correctedZ = (sensorData.gyroZ - gyroBiasZ) * GYRO_Z_AXIS_SIGN;

  // Dead-zone: discard noise below 0.01 rad/s (~0.6 deg/s)
  if (fabsf(correctedZ) < 0.01f) correctedZ = 0.0f;

  float gyroHeading = wrapHeading(fusedHeading + correctedZ * RAD_TO_DEG * dt);

  // ── Step 2 & 3: Magnetometer read + low-pass smooth ─────
  if (sensorData.magValid) {
    float rawMag = readMagHeading();

    // Low-pass: blend new mag reading toward smoothed value
    // MAG_SMOOTH_W = 0.80 → 80% old, 20% new per cycle
    magHeadingSmoothed = wrapHeading(
      magHeadingSmoothed + angleDiff(rawMag, magHeadingSmoothed) * (1.0f - MAG_SMOOTH_W)
    );

    // ── Step 4: Complementary blend ──────────────────────
    float correction = angleDiff(magHeadingSmoothed, gyroHeading);
    fusedHeading = wrapHeading(gyroHeading + (1.0f - COMP_ALPHA) * correction);
    headingSource = HEADING_COMP;

  } else {
    // No mag available — fall back to pure gyro dead-reckoning
    fusedHeading = gyroHeading;
    if (lastGpsLockMs == 0) {
      headingSource = HEADING_NONE;
    } else {
      headingSource = (millis() - lastGpsLockMs > GYRO_STALE_MS)
                      ? HEADING_STALE
                      : HEADING_GYRO;
    }
  }
}

// ============================================================
//  NAVIGATION ALGORITHM
// ============================================================
/*
 *  Simple bearing-line model:
 *
 *      Draw a straight line from current GPS position to base.
 *      That gives bearingToBase (absolute, 0-360).
 *
 *      relativeBearing = bearingToBase - fusedHeading
 *                        (normalised to -180..+180)
 *
 *      Negative = base is to your LEFT  → vibrate LEFT
 *      Positive = base is to your RIGHT → vibrate RIGHT
 *      Near zero (±45°) = you're aligned → FRONT pulses
 *      Behind (>±135°)  = turn around    → BACK steady
 *
 *  Only ONE motor fires at a time. Pulse timer per state.
 *  State change resets nothing — timers are independent.
 */
void updateNavigation() {
  // Always start clean
  allMotorsOff();

  // ── Run complementary filter even with no GPS fix ─────────
  // Mag+gyro keep heading valid indoors / stationary
  if (!sensorData.gpsValid) {
    updateComplementaryFilter();
    navState = NAV_NO_GPS;
    return;
  }

  // ── Always compute bearing & distance (even when ARRIVED) ─
  bearingToBase  = calcBearing(
    sensorData.latitude, sensorData.longitude, BASE_LAT, BASE_LNG);
  distanceToBase = calcDistance(
    sensorData.latitude, sensorData.longitude, BASE_LAT, BASE_LNG);

  // ── Complementary filter runs every cycle (mag + gyro) ───
  updateComplementaryFilter();

  // ── GPS COG snaps heading to true North when moving ───────
  // GPS is the most accurate absolute reference when valid
  if (sensorData.courseValid && sensorData.speed >= MIN_SPEED_KMH) {
    lockHeadingFromGPS(sensorData.gpsCourse);
  }

  // ── Always compute relativeBearing (fixes stale 0.0 bug) ──
  relativeBearing = normAngle(bearingToBase - fusedHeading);

  // ── ARRIVED: within radius → all motors fast pulse ────────
  if (distanceToBase <= ARRIVED_RADIUS_M) {
    navState = NAV_ARRIVED;
    bool p = tickArrivedPulse();
    for (int i = 0; i < 4; i++) digitalWrite(MOTORS[i], p ? HIGH : LOW);
    return;
  }

  // ── Directional zones (simple angle check) ────────────────
  float absRel = fabs(relativeBearing);

  if (absRel <= FRONT_ZONE_DEG) {
    // ── ALIGNED: base is ahead within 45° cone ─────────────
    // Pulsing FRONT = "on track, keep walking"
    navState = NAV_ALIGN_FRONT;
    digitalWrite(MOTOR_FRONT, tickFrontPulse() ? HIGH : LOW);

  } else if (relativeBearing > FRONT_ZONE_DEG && relativeBearing <= SIDE_ZONE_DEG) {
    // ── BASE IS TO THE RIGHT: +45° to +135° ────────────────
    // Steady RIGHT = "turn right to face base"
    navState = NAV_TURN_RIGHT;
    digitalWrite(MOTOR_RIGHT, HIGH);

  } else if (absRel > SIDE_ZONE_DEG) {
    // ── BASE IS BEHIND: beyond ±135° ───────────────────────
    // Steady BACK = "turn around"
    navState = NAV_TURN_BACK;
    digitalWrite(MOTOR_BACK, HIGH);

  } else {
    // ── BASE IS TO THE LEFT: -45° to -135° ─────────────────
    // Steady LEFT = "turn left to face base"
    navState = NAV_TURN_LEFT;
    digitalWrite(MOTOR_LEFT, HIGH);
  }
}

// ============================================================
//  WIFI
// ============================================================
void connectWiFi() {
  Serial.println("\n[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WiFi] Connected  IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("[WiFi] Failed (will retry)");
}

// ============================================================
//  GPS
// ============================================================
void initGPS() {
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(100);
  Serial.println("[GPS]  NEO-M8N ready  (RX=16 TX=17 @ 9600)");
  Serial.println("       Waiting for satellite lock...");
}

void readGPS() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  sensorData.satellites = gps.satellites.value();

  if (gps.location.isValid()) {
    sensorData.latitude   = gps.location.lat();
    sensorData.longitude  = gps.location.lng();
    sensorData.altitude   = gps.altitude.meters();
    sensorData.speed      = gps.speed.kmph();
    sensorData.gpsValid   = true;
  } else {
    sensorData.gpsValid   = false;
  }

  if (gps.course.isValid() && sensorData.speed >= MIN_SPEED_KMH) {
    sensorData.gpsCourse   = gps.course.deg();
    sensorData.courseValid = true;
  } else {
    sensorData.courseValid = false;
  }
}

// ============================================================
//  MAGNETOMETER (HMC5983)
// ============================================================
void initMag() {
  // HMC5983 shares the IMU I2C bus (Wire, SDA=21 SCL=22)
  // Wire.begin() is called in initIMU(), so just begin the sensor here
  if (!mag.begin()) {
    Serial.println("[MAG]  HMC5983 NOT FOUND -- check wiring! (SDA=21 SCL=22 addr=0x1E)");
    sensorData.magValid = false;
    return;
  }
  sensorData.magValid = true;
  Serial.println("[MAG]  HMC5983 ready  (SDA=21 SCL=22)");

  // Seed the smoothed heading with first reading (no tilt comp yet, accel not read)
  sensors_event_t event;
  mag.getEvent(&event);
  float mx = event.magnetic.x - MAG_OFFSET_X;
  float my = event.magnetic.y - MAG_OFFSET_Y;
  magHeadingSmoothed = wrapHeading(atan2f(my, mx) * RAD_TO_DEG);
  fusedHeading       = magHeadingSmoothed;  // start heading from mag at boot
  Serial.printf("[MAG]  Initial heading: %.1f deg\n", fusedHeading);
}

// ============================================================
//  IMU
// ============================================================
void initIMU() {
  Wire.begin(IMU_SDA, IMU_SCL);
  delay(100);
  if (!mpu.begin()) {
    Serial.println("[IMU]  MPU6050 NOT FOUND -- check wiring!");
    sensorData.imuValid = false;
    return;
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  delay(200);
  sensorData.imuValid = true;
  Serial.println("[IMU]  MPU6050 ready  (SDA=21 SCL=22)");

  // Bias calibration -- device must be still during this
  calibrateGyroBias();
}

void readIMU() {
  if (!sensorData.imuValid) return;
  sensors_event_t accel, gyro, temp;
  mpu.getEvent(&accel, &gyro, &temp);
  sensorData.accelX      = accel.acceleration.x;
  sensorData.accelY      = accel.acceleration.y;
  sensorData.accelZ      = accel.acceleration.z;
  sensorData.gyroX       = gyro.gyro.x;
  sensorData.gyroY       = gyro.gyro.y;
  sensorData.gyroZ       = gyro.gyro.z;
  sensorData.temperature = temp.temperature;
}

// ============================================================
//  MAX30102 PULSE OXIMETER
// ============================================================
void initMAX30102() {
  // MAX30102 needs its own I2C bus on Wire1 (GPIO 32/33)
  Wire1.begin(MAX_SDA, MAX_SCL);
  delay(100);

  if (!particleSensor.begin(Wire1, I2C_SPEED_FAST)) {
    Serial.println("[MAX]  MAX30102 NOT FOUND -- check wiring!");
    Serial.println("       SDA -> GPIO 32   SCL -> GPIO 33");
    sensorData.maxValid = false;
    return;
  }

  // Configure for heart rate mode
  // ledBrightness: 60mA, sampleAverage: 4, ledMode: RED+IR,
  // sampleRate: 100Hz, pulseWidth: 411us, adcRange: 4096
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x0A);  // Low red for proximity
  particleSensor.setPulseAmplitudeGreen(0);   // Green off

  sensorData.maxValid = true;
  Serial.println("[MAX]  MAX30102 ready  (SDA=32 SCL=33)");
}

void readMAX30102() {
  if (!sensorData.maxValid) return;

  long irValue = particleSensor.getIR();
  sensorData.irValue = (float)irValue;

  // Finger detection: IR > 50000 means finger is present
  sensorData.fingerOn = (irValue > 50000);

  if (sensorData.fingerOn) {
    // Run beat detector on IR value
    if (checkForBeat(irValue)) {
      long now   = millis();
      long delta = now - hrLastBeat;
      hrLastBeat = now;

      hrBPM = 60.0f / (delta / 1000.0f);

      // Sanity check: only store if plausible heart rate
      if (hrBPM >= 30 && hrBPM <= 220) {
        hrRates[hrRateIdx++] = (byte)hrBPM;
        hrRateIdx %= HR_SAMPLE_SIZE;

        // Compute rolling average
        float sum = 0;
        for (int i = 0; i < HR_SAMPLE_SIZE; i++) sum += hrRates[i];
        hrAvgBPM = sum / HR_SAMPLE_SIZE;
        sensorData.heartRate = hrAvgBPM;
      }
    }
  } else {
    // No finger — reset readings
    sensorData.heartRate = 0.0f;
    hrBPM    = 0.0f;
    hrAvgBPM = 0.0f;
    for (int i = 0; i < HR_SAMPLE_SIZE; i++) hrRates[i] = 0;
  }
}


const char* navLabel(NavState s) {
  switch(s) {
    case NAV_NO_GPS:      return "NO GPS FIX      ";
    case NAV_INIT:        return "WALK TO INIT    ";
    case NAV_ALIGN_FRONT: return ">> GO FORWARD <<";
    case NAV_TURN_RIGHT:  return ">> TURN RIGHT   ";
    case NAV_TURN_BACK:   return ">> TURN BACK    ";
    case NAV_TURN_LEFT:   return ">> TURN LEFT    ";
    case NAV_ARRIVED:     return "** ARRIVED **   ";
    default:              return "UNKNOWN         ";
  }
}

const char* headingLabel(HeadingSource s) {
  switch(s) {
    case HEADING_NONE:  return "BOOT ";
    case HEADING_GPS:   return "GPS  ";
    case HEADING_COMP:  return "COMP ";
    case HEADING_GYRO:  return "GYRO ";
    case HEADING_STALE: return "STALE";
    default:            return "?    ";
  }
}

// ============================================================
//  HTTP TELEMETRY
// ============================================================
void sendSensorData() {
  packetsSent++;

  Serial.println("\n============================================================");
  Serial.printf(" PKT #%-4d | %s\n", packetsSent, navLabel(navState));
  Serial.println("------------------------------------------------------------");

  if (sensorData.gpsValid) {
    Serial.printf(" GPS  | %.6f, %.6f | Alt:%.1fm Spd:%.1fkm/h Sats:%d\n",
      sensorData.latitude, sensorData.longitude,
      sensorData.altitude, sensorData.speed, sensorData.satellites);
    if (sensorData.courseValid)
      Serial.printf("      | GPS Course: %.1f deg  (moving)\n", sensorData.gpsCourse);
    else
      Serial.printf("      | GPS Course: --  (speed below %.1f km/h)\n", MIN_SPEED_KMH);
  } else {
    Serial.printf(" GPS  | NO FIX  Sats: %d\n", sensorData.satellites);
  }

  Serial.printf(" HDG  | [%s] Fused: %6.1f deg  "
                "ToBase: %6.1f deg  Rel: %+6.1f deg  Dist: %.1fm\n",
    headingLabel(headingSource),
    fusedHeading, bearingToBase, relativeBearing, distanceToBase);

  if (headingSource == HEADING_STALE) {
    Serial.printf("      | !! STALE: no GPS lock for %lu s -- gyro drift likely\n",
      (millis() - lastGpsLockMs) / 1000UL);
  }

  if (sensorData.imuValid) {
    Serial.printf(" IMU  | Accel: X=%6.2f Y=%6.2f Z=%6.2f m/s2\n",
      sensorData.accelX, sensorData.accelY, sensorData.accelZ);
    Serial.printf("      | Gyro:  X=%6.2f Y=%6.2f Z=%6.2f rad/s"
                  "  bias=%.5f  Temp:%.1fC\n",
      sensorData.gyroX, sensorData.gyroY, sensorData.gyroZ,
      gyroBiasZ, sensorData.temperature);
  } else {
    Serial.println(" IMU  | NOT DETECTED");
  }

  if (sensorData.magValid) {
    Serial.printf(" MAG  | HMC5983 active  Smoothed: %.1f deg\n", magHeadingSmoothed);
  } else {
    Serial.println(" MAG  | HMC5983 NOT DETECTED -- complementary filter disabled");
  }

  if (sensorData.maxValid) {
    if (sensorData.fingerOn) {
      Serial.printf(" HR   | Finger: YES  BPM: %.1f  (raw IR: %.0f)\n",
        sensorData.heartRate, sensorData.irValue);
    } else {
      Serial.printf(" HR   | Finger: NO   (IR: %.0f -- place finger on sensor)\n",
        sensorData.irValue);
    }
  } else {
    Serial.println(" HR   | MAX30102 NOT DETECTED");
  }

  Serial.println("------------------------------------------------------------");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" HTTP | Not connected -- skipping");
    Serial.println("============================================================");
    return;
  }

  StaticJsonDocument<900> doc;

  JsonObject gpsObj = doc.createNestedObject("gps");
  gpsObj["lat"]        = sensorData.latitude;
  gpsObj["lng"]        = sensorData.longitude;
  gpsObj["alt"]        = sensorData.altitude;
  gpsObj["speed"]      = sensorData.speed;
  gpsObj["course"]     = sensorData.courseValid ? sensorData.gpsCourse : -1.0f;
  gpsObj["satellites"] = sensorData.satellites;
  gpsObj["valid"]      = sensorData.gpsValid;

  JsonObject imuObj = doc.createNestedObject("imu");
  imuObj["accel_x"] = sensorData.accelX;
  imuObj["accel_y"] = sensorData.accelY;
  imuObj["accel_z"] = sensorData.accelZ;
  imuObj["gyro_x"]  = sensorData.gyroX;
  imuObj["gyro_y"]  = sensorData.gyroY;
  imuObj["gyro_z"]  = sensorData.gyroZ;
  imuObj["temp"]    = sensorData.temperature;
  imuObj["valid"]   = sensorData.imuValid;

  JsonObject navObj = doc.createNestedObject("nav");
  navObj["fused_heading"] = fusedHeading;
  navObj["heading_src"]   = headingLabel(headingSource);
  navObj["bearing"]       = bearingToBase;
  navObj["rel_bearing"]   = relativeBearing;
  navObj["distance"]      = distanceToBase;
  navObj["state"]         = navLabel(navState);

  JsonObject hrObj = doc.createNestedObject("hr");
  hrObj["bpm"]       = sensorData.heartRate;
  hrObj["ir"]        = sensorData.irValue;
  hrObj["finger"]    = sensorData.fingerOn;
  hrObj["valid"]     = sensorData.maxValid;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(BASE_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  if (code > 0) {
    Serial.printf(" HTTP | %d", code);
    if (code == 200) {
      Serial.print("  ok");
      digitalWrite(STATUS_LED, HIGH); delay(60); digitalWrite(STATUS_LED, LOW);
    }
    Serial.println();
  } else {
    Serial.printf(" HTTP | FAILED: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  Serial.println("============================================================");
}

// ============================================================
//  STARTUP MOTOR TEST
// ============================================================
void motorStartupTest() {
  Serial.println("[MOTORS] Startup test...");
  const int   pins[]  = {MOTOR_FRONT, MOTOR_RIGHT, MOTOR_BACK, MOTOR_LEFT};
  const char* names[] = {"FRONT", "RIGHT", "BACK", "LEFT"};
  for (int i = 0; i < 4; i++) {
    Serial.printf("         %s\n", names[i]);
    digitalWrite(pins[i], HIGH); delay(300);
    digitalWrite(pins[i], LOW);  delay(100);
  }
  Serial.println("[MOTORS] Done");
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n============================================================");
  Serial.println("  NAVIGATION BELT v3.2");
  Serial.println("  GPS + HMC5983 Mag + Gyro Complementary Filter + Heart Rate");
  Serial.println("============================================================");
  Serial.printf("  Base station:  Colombo Fort Railway Station\n");
  Serial.printf("                 %.6f, %.6f\n", BASE_LAT, BASE_LNG);
  Serial.printf("  Arrived at:    %.1f m\n", ARRIVED_RADIUS_M);
  Serial.printf("  Gyro stale:    after %d s without GPS lock\n", GYRO_STALE_MS/1000);
  Serial.println("============================================================\n");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  for (int i = 0; i < 4; i++) {
    pinMode(MOTORS[i], OUTPUT);
    digitalWrite(MOTORS[i], LOW);
  }

  connectWiFi();
  initGPS();
  initIMU();       // includes gyro bias calibration -- keep still!
  initMag();       // HMC5983: seeds initial heading from compass
  initMAX30102();

  lastGyroUpdateMs = millis();

  Serial.println("\n[READY] Compass heading active from boot.");
  Serial.println("        Complementary filter: HMC5983 mag + gyro fused.");
  Serial.println("        GPS COG snaps to true North when walking.\n");
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  // Feed GPS parser continuously
  readGPS();

  // Read raw IMU (gyroZ used inside updateComplementaryFilter())
  readIMU();

  // Read pulse oximeter
  readMAX30102();

  // Navigation + heading fusion at 10 Hz
  if (millis() - lastNav >= NAV_UPDATE_MS) {
    updateNavigation();
    lastNav = millis();
  }

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    connectWiFi();
  }

  // Telemetry
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    sendSensorData();
    lastSend = millis();
  }

  delay(5);
}