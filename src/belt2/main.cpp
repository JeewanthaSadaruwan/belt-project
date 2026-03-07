/*
 * BELT 2 - Sensor Reader + HTTP Sender
 * Reads GPS and IMU sensors and sends to Base Station via HTTP POST
 *
 * Hardware:
 * - GPS NEO-M8N on UART2 (RX=16, TX=17)
 * - MPU6050 IMU on I2C (SDA=21, SCL=22)
 * - Status LED on GPIO 2
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

// ========== WIFI / BASE STATION ==========
#define WIFI_SSID        "BeltStation"
#define WIFI_PASSWORD    "belt12345"
#define BASE_URL         "http://192.168.4.1/api/data"

// ========== PIN DEFINITIONS ==========
// GPS NEO-M8N (UART)
#define GPS_RX              16  // ESP32 RX (connects to GPS TX)
#define GPS_TX              17  // ESP32 TX (connects to GPS RX)
#define GPS_BAUD            9600

// IMU MPU6050 (I2C)
#define IMU_SDA             21
#define IMU_SCL             22

// Status LED
#define STATUS_LED          2

// ========== TIMING ==========
#define SEND_INTERVAL       2000   // Send data every 2 seconds
#define WIFI_RETRY_MS       5000

// ========== GLOBAL OBJECTS ==========
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);  // Use UART2
Adafruit_MPU6050 mpu;

// ========== DATA STRUCTURE (matches Base Station) ==========
typedef struct {
  // GPS data
  double latitude;
  double longitude;
  float altitude;
  float speed;
  uint8_t satellites;
  bool gpsValid;
  
  // IMU data (Accelerometer)
  float accelX;
  float accelY;
  float accelZ;
  
  // IMU data (Gyroscope)
  float gyroX;
  float gyroY;
  float gyroZ;
  
  // Temperature
  float temperature;
  bool imuValid;
} SensorData;

SensorData sensorData;
unsigned long lastSend = 0;
int packetsSent = 0;
unsigned long lastWifiAttempt = 0;

// ========== WIFI FUNCTIONS ==========
void connectWiFi() {
  Serial.println("\n[WiFi] Connecting to Base Station...");
  Serial.printf("   SSID: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected!");
    Serial.printf("   IP Address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] Connection failed (will retry)");
  }
}


// ========== GPS FUNCTIONS ==========
void initGPS() {
  Serial.println("\n[GPS] Initializing NEO-M8N...");
  Serial.printf("   RX Pin: GPIO %d (connects to GPS TX)\n", GPS_RX);
  Serial.printf("   TX Pin: GPIO %d (connects to GPS RX)\n", GPS_TX);
  Serial.printf("   Baud Rate: %d\n", GPS_BAUD);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(100);

  Serial.println("[GPS] Serial initialized");
  Serial.println("   Waiting for GPS fix (may take 1-5 minutes outdoors)...");
  Serial.println("   Note: GPS needs clear sky view to get satellite lock");
}


void readGPS() {
  // Read all available GPS data
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
  }
  
  // Update readings
  if (gps.location.isValid()) {
    sensorData.latitude = gps.location.lat();
    sensorData.longitude = gps.location.lng();
    sensorData.altitude = gps.altitude.meters();
    sensorData.speed = gps.speed.kmph();
    sensorData.satellites = gps.satellites.value();
    sensorData.gpsValid = true;
  } else {
    sensorData.satellites = gps.satellites.value();
    sensorData.gpsValid = false;
  }
}

// ========== IMU FUNCTIONS ==========
void initIMU() {
  Serial.println("\n[IMU] Initializing MPU6050...");
  Serial.printf("   SDA Pin: GPIO %d\n", IMU_SDA);
  Serial.printf("   SCL Pin: GPIO %d\n", IMU_SCL);

  // Initialize I2C bus
  Wire.begin(IMU_SDA, IMU_SCL);
  delay(100);

  // Try to initialize MPU6050
  if (!mpu.begin()) {
    Serial.println("[IMU] MPU6050 not found!");
    Serial.println("   Check wiring:");
    Serial.println("   - VCC -> 3.3V");
    Serial.println("   - GND -> GND");
    Serial.println("   - SDA -> GPIO 21");
    Serial.println("   - SCL -> GPIO 22");
    sensorData.imuValid = false;
    return;
  }

  Serial.println("[IMU] MPU6050 found");

  // Configure sensor ranges
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  Serial.println("   Accelerometer range: +/- 8G");

  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  Serial.println("   Gyroscope range: +/- 500 deg/s");

  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("   Filter bandwidth: 21 Hz");

  delay(100);
  sensorData.imuValid = true;
}


void readIMU() {
  if (!sensorData.imuValid) {
    return;
  }
  
  // Get new sensor events
  sensors_event_t accel, gyro, temp;
  mpu.getEvent(&accel, &gyro, &temp);
  
  // Update accelerometer readings (m/s^2)
  sensorData.accelX = accel.acceleration.x;
  sensorData.accelY = accel.acceleration.y;
  sensorData.accelZ = accel.acceleration.z;
  
  // Update gyroscope readings (rad/s)
  sensorData.gyroX = gyro.gyro.x;
  sensorData.gyroY = gyro.gyro.y;
  sensorData.gyroZ = gyro.gyro.z;
  
  // Update temperature (C)
  sensorData.temperature = temp.temperature;
}

// ========== SEND SENSOR DATA VIA HTTP ==========
void sendSensorData() {
  packetsSent++;

  Serial.println("\n============================================================");
  Serial.printf("SENDING DATA PACKET #%d via HTTP\n", packetsSent);
  Serial.println("============================================================");

  // ===== GPS DATA =====
  Serial.println("\nGPS DATA:");
  Serial.println("------------------------------------------------------------");

  if (sensorData.gpsValid) {
    Serial.println("   Status:      VALID FIX");
    Serial.printf("   Latitude:    %.6f deg\n", sensorData.latitude);
    Serial.printf("   Longitude:   %.6f deg\n", sensorData.longitude);
    Serial.printf("   Altitude:    %.1f m\n", sensorData.altitude);
    Serial.printf("   Speed:       %.1f km/h\n", sensorData.speed);
    Serial.printf("   Satellites:  %d\n", sensorData.satellites);
  } else {
    Serial.println("   Status:      NO FIX");
    Serial.printf("   Satellites:  %d (waiting for lock...)\n", sensorData.satellites);
    Serial.println("   Note:        GPS needs clear sky view");
  }

  // ===== IMU DATA =====
  Serial.println("\nIMU DATA (MPU6050):");
  Serial.println("------------------------------------------------------------");

  if (sensorData.imuValid) {
    Serial.println("   Status:      ACTIVE");

    Serial.println("\n   Accelerometer (m/s^2):");
    Serial.printf("      X-axis:   %7.2f\n", sensorData.accelX);
    Serial.printf("      Y-axis:   %7.2f\n", sensorData.accelY);
    Serial.printf("      Z-axis:   %7.2f\n", sensorData.accelZ);

    Serial.println("\n   Gyroscope (rad/s):");
    Serial.printf("      X-axis:   %7.2f\n", sensorData.gyroX);
    Serial.printf("      Y-axis:   %7.2f\n", sensorData.gyroY);
    Serial.printf("      Z-axis:   %7.2f\n", sensorData.gyroZ);

    Serial.println("\n   Temperature:");
    Serial.printf("      Temp:     %.1f C\n", sensorData.temperature);
  } else {
    Serial.println("   Status:      NOT DETECTED");
    Serial.println("   Action:      Check I2C connections");
  }

  Serial.println("\n------------------------------------------------------------");

  // Send via HTTP POST
  if (WiFi.status() == WL_CONNECTED) {
    StaticJsonDocument<512> doc;
    JsonObject gps = doc.createNestedObject("gps");
    gps["lat"] = sensorData.latitude;
    gps["lng"] = sensorData.longitude;
    gps["alt"] = sensorData.altitude;
    gps["speed"] = sensorData.speed;
    gps["satellites"] = sensorData.satellites;
    gps["valid"] = sensorData.gpsValid;

    JsonObject imu = doc.createNestedObject("imu");
    imu["accel_x"] = sensorData.accelX;
    imu["accel_y"] = sensorData.accelY;
    imu["accel_z"] = sensorData.accelZ;
    imu["gyro_x"] = sensorData.gyroX;
    imu["gyro_y"] = sensorData.gyroY;
    imu["gyro_z"] = sensorData.gyroZ;
    imu["temp"] = sensorData.temperature;
    imu["valid"] = sensorData.imuValid;

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    http.begin(BASE_URL);
    http.addHeader("Content-Type", "application/json");

    Serial.print("\nSending to Base Station... ");
    int httpCode = http.POST(payload);
    if (httpCode > 0) {
      Serial.printf("[HTTP %d]\n", httpCode);
      String resp = http.getString();
      if (httpCode == 200) {
        // Blink LED on success
        digitalWrite(STATUS_LED, HIGH);
        delay(100);
        digitalWrite(STATUS_LED, LOW);
      } else {
        Serial.printf("[HTTP] Response: %s\n", resp.c_str());
      }
    } else {
      Serial.printf("[FAILED] %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("\n[WiFi] Not connected - cannot send data");
  }

  Serial.println();
}


// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(2000);  // Wait for serial monitor

  Serial.println("\n\n");
  Serial.println("============================================================");
  Serial.println("BELT 2 - HTTP SENSOR TRANSMITTER");
  Serial.println("GPS + IMU -> Base Station via WiFi HTTP");
  Serial.println("============================================================");

  // Setup LED
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  // Initialize WiFi first
  connectWiFi();

  // Initialize sensors
  initGPS();
  initIMU();

  Serial.println("\n");
  Serial.println("INITIALIZATION COMPLETE");
  Serial.println("");
  Serial.println("Sending sensor data to Base Station every 2 seconds...");
  Serial.println("   (GPS may take a few minutes to get first fix)");
  Serial.println("\n");
}


// ========== MAIN LOOP ==========
void loop() {
  // Continuously read GPS (it needs constant feeding)
  readGPS();
  
  // Read IMU data
  readIMU();
  
  // Reconnect WiFi if needed
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    connectWiFi();
  }

  // Send sensor data at regular intervals
  if (millis() - lastSend >= SEND_INTERVAL) {
    sendSensorData();
    lastSend = millis();
  }
  
  // Small delay to prevent hogging CPU
  delay(10);
}
