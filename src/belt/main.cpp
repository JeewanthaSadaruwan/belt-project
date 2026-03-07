/*
 * BELT DEVICE FIRMWARE - CONSOLIDATED VERSION
 * Wearable belt with GPS, IMU sensors and motor control
 * 
 * Features:
 * - LoRa communication with base station
 * - GPS tracking (NEO-M8N)
 * - Motion sensing (MPU6050 IMU) 
 * - 4-motor control system (L298N driver)
 * - Real-time sensor data transmission
 */

// ========== LIBRARY INCLUDES ==========
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// ========== PIN DEFINITIONS ==========
// LoRa RA-02 Module Pins
#define LORA_CS             5
#define LORA_RST            14
#define LORA_DIO0           26
#define LORA_SCK            18
#define LORA_MISO           19
#define LORA_MOSI           23

// GPS NEO-M8N (UART)
#define GPS_RX              16  // ESP32 RX (connects to GPS TX)
#define GPS_TX              17  // ESP32 TX (connects to GPS RX)
#define GPS_BAUD            9600

// IMU MPU6050 (I2C)
#define IMU_SDA             21
#define IMU_SCL             22

// Motor Control - L298N Driver
// Left motors
#define LEFT_ENA            25  // PWM speed for left motors
#define LEFT_IN1            13  // Direction control 1
#define LEFT_IN2            33  // Direction control 2

// Right motors
#define RIGHT_ENB           4   // PWM speed for right motors
#define RIGHT_IN3           12  // Direction control 1
#define RIGHT_IN4           15  // Direction control 2

// Status LED
#define STATUS_LED          2

// ========== CONSTANTS AND CONFIGURATION ==========
// PWM Configuration for Motors
#define LEFT_ENA_CH         0   // PWM channel for left motors
#define RIGHT_ENB_CH        1   // PWM channel for right motors
#define PWM_FREQ            5000    // 5 kHz PWM frequency
#define PWM_RESOLUTION      8       // 8-bit resolution (0-255)

// LoRa Configuration
#ifndef LORA_FREQUENCY
#define LORA_FREQUENCY      433E6   // 433 MHz
#endif

// Packet Types
#define PKT_DATA            0x10
#define PKT_STATUS          0x11

// Timing Constants
#define SENSOR_TRANSMIT_INTERVAL 2000   // Transmit every 2 seconds

// ========== DATA STRUCTURES ==========
// Sensor readings structure
struct SensorReadings {
  // GPS data
  double latitude;
  double longitude;
  float altitude;
  float speed;
  uint8_t satellites;
  bool gpsValid;
  
  // IMU data
  float accelX;
  float accelY;
  float accelZ;
  float gyroX;
  float gyroY;
  float gyroZ;
  float temperature;
  bool imuValid;
  
  // Motor status
  int leftSpeed;
  int rightSpeed;
  String leftDir;
  String rightDir;
};

// ========== GLOBAL VARIABLES ==========
// LoRa
uint16_t loraPacketCounter = 0;

// GPS
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);  // Use Serial1

// IMU
Adafruit_MPU6050 mpu;

// Timing
unsigned long lastSensorTransmit = 0;

// Sensor status
bool gpsInitOK = false;
bool imuInitOK = false;

// Motor control
volatile int currentLeftSpeed = 0;
volatile int currentRightSpeed = 0;

// ========== FORWARD DECLARATIONS ==========
bool initLoRa();
bool initGPS();
bool initIMU();
void initMotors();
void sendDataViaLoRa();
SensorReadings readAllSensors();
void setMotorSpeed(int leftSpeed, int rightSpeed);
void driveMotorsForward(int speed);
void driveMotorsBackward(int speed);
void stopMotors();

// ========== LoRa FUNCTIONS ==========
bool initLoRa() {
  Serial.println("\n[LoRa] Initializing transmitter...");
  
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] ❌ Initialization failed!");
    return false;
  }
  
  // Configure LoRa parameters (matching base station)
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  
  Serial.print("[LoRa] ✅ Frequency: ");
  Serial.print(LORA_FREQUENCY / 1E6);
  Serial.println(" MHz");
  Serial.println("[LoRa] Transmitter ready!");
  return true;
}

// Send data via LoRa with packet header
bool sendLoRaPacket(uint8_t type, const char* data) {
  size_t dataLen = strlen(data);
  if (dataLen > 240) dataLen = 240;
  
  // Build packet: [type][counter_high][counter_low][length][data...]
  uint8_t buffer[4 + dataLen];
  buffer[0] = type;
  buffer[1] = (uint8_t)(loraPacketCounter >> 8);
  buffer[2] = (uint8_t)(loraPacketCounter & 0xFF);
  buffer[3] = (uint8_t)dataLen;
  memcpy(&buffer[4], data, dataLen);
  
  // Transmit
  LoRa.beginPacket();
  LoRa.write(buffer, 4 + dataLen);
  int result = LoRa.endPacket();
  
  loraPacketCounter++;
  return result;
}

// ========== GPS FUNCTIONS ==========
bool initGPS() {
  Serial.println("\n[GPS] Initializing NEO-M8N...");
  
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(1000);
  
  // Check if GPS is responding
  unsigned long start = millis();
  while (millis() - start < 2000) {
    while (gpsSerial.available()) {
      gps.encode(gpsSerial.read());
    }
    if (gps.charsProcessed() > 10) {
      gpsInitOK = true;
      Serial.println("[GPS] ✅ GPS module responding");
      Serial.println("[GPS] Waiting for satellite fix...");
      return true;
    }
  }
  
  Serial.println("[GPS] ⚠️ GPS not responding (will keep trying)");
  gpsInitOK = false;
  return false;
}

// Read GPS data
void readGPSData() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
}

// ========== IMU FUNCTIONS ==========
bool initIMU() {
  Serial.println("\n[IMU] Initializing MPU6050...");
  
  Wire.begin(IMU_SDA, IMU_SCL);
  Wire.setClock(400000);
  delay(100);
  
  if (!mpu.begin()) {
    Serial.println("[IMU] ❌ MPU6050 not found!");
    imuInitOK = false;
    return false;
  }
  
  // Configure IMU
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  
  Serial.println("[IMU] ✅ MPU6050 initialized");
  imuInitOK = true;
  return true;
}

// Read IMU data
void readIMUData(float& accelX, float& accelY, float& accelZ, 
                 float& gyroX, float& gyroY, float& gyroZ, float& temp) {
  if (imuInitOK) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    
    accelX = a.acceleration.x;
    accelY = a.acceleration.y;
    accelZ = a.acceleration.z;
    gyroX = g.gyro.x;
    gyroY = g.gyro.y;
    gyroZ = g.gyro.z;
    temp = t.temperature;
  } else {
    accelX = accelY = accelZ = 0;
    gyroX = gyroY = gyroZ = 0;
    temp = 0;
  }
}

// ========== MOTOR CONTROL FUNCTIONS ==========
void initMotors() {
  Serial.println("\n[Motors] Initializing L298N 4-motor system...");
  
  // Setup direction pins
  pinMode(LEFT_IN1, OUTPUT);
  pinMode(LEFT_IN2, OUTPUT);
  pinMode(RIGHT_IN3, OUTPUT);
  pinMode(RIGHT_IN4, OUTPUT);
  
  // Setup PWM channels
  ledcSetup(LEFT_ENA_CH, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(RIGHT_ENB_CH, PWM_FREQ, PWM_RESOLUTION);
  
  // Attach PWM pins
  ledcAttachPin(LEFT_ENA, LEFT_ENA_CH);
  ledcAttachPin(RIGHT_ENB, RIGHT_ENB_CH);
  
  // Stop all motors initially
  stopMotors();
  
  Serial.println("[Motors] ✅ 4-motor system initialized");
}

// Set motor speeds and directions
void setMotorSpeed(int leftSpeed, int rightSpeed) {
  // Constrain speeds
  leftSpeed = constrain(leftSpeed, -255, 255);
  rightSpeed = constrain(rightSpeed, -255, 255);
  
  // Left motors
  if (leftSpeed > 0) {
    // Forward
    digitalWrite(LEFT_IN1, HIGH);
    digitalWrite(LEFT_IN2, LOW);
    ledcWrite(LEFT_ENA_CH, leftSpeed);
  } else if (leftSpeed < 0) {
    // Backward
    digitalWrite(LEFT_IN1, LOW);
    digitalWrite(LEFT_IN2, HIGH);
    ledcWrite(LEFT_ENA_CH, -leftSpeed);
  } else {
    // Stop
    digitalWrite(LEFT_IN1, LOW);
    digitalWrite(LEFT_IN2, LOW);
    ledcWrite(LEFT_ENA_CH, 0);
  }
  
  // Right motors
  if (rightSpeed > 0) {
    // Forward
    digitalWrite(RIGHT_IN3, HIGH);
    digitalWrite(RIGHT_IN4, LOW);
    ledcWrite(RIGHT_ENB_CH, rightSpeed);
  } else if (rightSpeed < 0) {
    // Backward
    digitalWrite(RIGHT_IN3, LOW);
    digitalWrite(RIGHT_IN4, HIGH);
    ledcWrite(RIGHT_ENB_CH, -rightSpeed);
  } else {
    // Stop
    digitalWrite(RIGHT_IN3, LOW);
    digitalWrite(RIGHT_IN4, LOW);
    ledcWrite(RIGHT_ENB_CH, 0);
  }
  
  currentLeftSpeed = leftSpeed;
  currentRightSpeed = rightSpeed;
}

// Drive all motors forward
void driveMotorsForward(int speed) {
  speed = constrain(speed, 0, 255);
  setMotorSpeed(speed, speed);
  Serial.print("[Motors] Forward at speed: ");
  Serial.println(speed);
}

// Drive all motors backward
void driveMotorsBackward(int speed) {
  speed = constrain(speed, 0, 255);
  setMotorSpeed(-speed, -speed);
  Serial.print("[Motors] Backward at speed: ");
  Serial.println(speed);
}

// Stop all motors
void stopMotors() {
  digitalWrite(LEFT_IN1, LOW);
  digitalWrite(LEFT_IN2, LOW);
  digitalWrite(RIGHT_IN3, LOW);
  digitalWrite(RIGHT_IN4, LOW);
  ledcWrite(LEFT_ENA_CH, 0);
  ledcWrite(RIGHT_ENB_CH, 0);
  currentLeftSpeed = 0;
  currentRightSpeed = 0;
}

// ========== SENSOR READING FUNCTIONS ==========
SensorReadings readAllSensors() {
  SensorReadings readings;
  
  // Read GPS
  readGPSData();
  if (gps.location.isValid()) {
    readings.gpsValid = true;
    readings.latitude = gps.location.lat();
    readings.longitude = gps.location.lng();
    readings.altitude = gps.altitude.meters();
    readings.speed = gps.speed.kmph();
    readings.satellites = gps.satellites.value();
  } else {
    readings.gpsValid = false;
    readings.latitude = 0.0;
    readings.longitude = 0.0;
    readings.altitude = 0.0;
    readings.speed = 0.0;
    readings.satellites = 0;
  }
  
  // Read IMU
  float accelX, accelY, accelZ, gyroX, gyroY, gyroZ, temp;
  readIMUData(accelX, accelY, accelZ, gyroX, gyroY, gyroZ, temp);
  
  readings.imuValid = imuInitOK;
  readings.accelX = accelX;
  readings.accelY = accelY;
  readings.accelZ = accelZ;
  readings.gyroX = gyroX;
  readings.gyroY = gyroY;
  readings.gyroZ = gyroZ;
  readings.temperature = temp;
  
  // Motor status
  readings.leftSpeed = abs(currentLeftSpeed);
  readings.rightSpeed = abs(currentRightSpeed);
  
  if (currentLeftSpeed > 0) readings.leftDir = "forward";
  else if (currentLeftSpeed < 0) readings.leftDir = "backward";
  else readings.leftDir = "stop";
  
  if (currentRightSpeed > 0) readings.rightDir = "forward";
  else if (currentRightSpeed < 0) readings.rightDir = "backward";
  else readings.rightDir = "stop";
  
  return readings;
}

// ========== DATA TRANSMISSION ==========
void sendDataViaLoRa() {
  SensorReadings readings = readAllSensors();
  
  // Build JSON-like string payload
  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"gps\":{\"valid\":%s,\"lat\":%.6f,\"lng\":%.6f,\"alt\":%.1f,\"speed\":%.1f,\"sats\":%d},"
    "\"imu\":{\"valid\":%s,\"accel\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
    "\"gyro\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},\"temp\":%.1f},"
    "\"motors\":{\"left_speed\":%d,\"left_dir\":\"%s\",\"right_speed\":%d,\"right_dir\":\"%s\"}}",
    readings.gpsValid ? "true" : "false",
    readings.latitude, readings.longitude, readings.altitude, 
    readings.speed, readings.satellites,
    readings.imuValid ? "true" : "false",
    readings.accelX, readings.accelY, readings.accelZ,
    readings.gyroX, readings.gyroY, readings.gyroZ,
    readings.temperature,
    readings.leftSpeed, readings.leftDir.c_str(),
    readings.rightSpeed, readings.rightDir.c_str());
  
  // Print summary
  Serial.println("\n📡 ═══════════════════════════════════════");
  Serial.println("    TRANSMITTING SENSOR DATA");
  Serial.println("═══════════════════════════════════════");
  
  if (readings.gpsValid) {
    Serial.printf("GPS: %.6f, %.6f (%.1f m, %d sats)\n",
                  readings.latitude, readings.longitude,
                  readings.altitude, readings.satellites);
  } else {
    Serial.println("GPS: No fix");
  }
  
  if (readings.imuValid) {
    Serial.printf("IMU: Accel(%.2f, %.2f, %.2f) m/s²\n",
                  readings.accelX, readings.accelY, readings.accelZ);
    Serial.printf("     Gyro(%.2f, %.2f, %.2f) rad/s\n",
                  readings.gyroX, readings.gyroY, readings.gyroZ);
    Serial.printf("     Temp: %.1f°C\n", readings.temperature);
  } else {
    Serial.println("IMU: Not available");
  }
  
  Serial.printf("Motors: L=%d(%s), R=%d(%s)\n",
                readings.leftSpeed, readings.leftDir.c_str(),
                readings.rightSpeed, readings.rightDir.c_str());
  
  Serial.printf("Packet #%d | Size: %d bytes\n", 
                loraPacketCounter, strlen(payload));
  
  // Send via LoRa
  if (sendLoRaPacket(PKT_DATA, payload)) {
    Serial.println("✓ Transmission successful!");
    digitalWrite(STATUS_LED, HIGH);
    delay(50);
    digitalWrite(STATUS_LED, LOW);
  } else {
    Serial.println("✗ Transmission FAILED!");
  }
  
  Serial.println("═══════════════════════════════════════\n");
}

// ========== SETUP AND LOOP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║       BELT DEVICE - Starting         ║");
  Serial.println("║   GPS + IMU + Motors + LoRa TX       ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println();
  
  // Status LED
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  
  // Initialize LoRa (critical)
  if (!initLoRa()) {
    Serial.println("[FATAL] LoRa failed - halted");
    while (1) {
      digitalWrite(STATUS_LED, HIGH);
      delay(200);
      digitalWrite(STATUS_LED, LOW);
      delay(200);
    }
  }
  
  // Initialize GPS (non-critical)
  initGPS();
  
  // Initialize IMU (non-critical)
  initIMU();
  
  // Initialize Motors
  initMotors();
  
  Serial.println("\n✅ Belt Device Ready!");
  Serial.println("📡 Transmitting sensor data every 2 seconds...\n");
  
  // Simple motor test - drive forward briefly then stop
  Serial.println("[Motors] Running startup test...");
  driveMotorsForward(150);
  delay(1000);
  stopMotors();
  Serial.println("[Motors] Test complete\n");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Continuously update GPS data
  readGPSData();
  
  // Transmit sensor data periodically
  if (currentTime - lastSensorTransmit >= SENSOR_TRANSMIT_INTERVAL) {
    lastSensorTransmit = currentTime;
    sendDataViaLoRa();
  }
  
  // You can add motor control logic here
  // Example: Control motors based on time or sensor data
  
  delay(100);
}
