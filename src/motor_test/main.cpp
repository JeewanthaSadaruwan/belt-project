/*
 * MOTOR TEST - Vibrator Motor Wiring Test
 * Tests all 4 vibrator motors connected via 2x L298N modules
 * 
 * Wiring:
 *   L298N #1:  IN1 -> GPIO 25 (FRONT)
 *              IN2 -> GND
 *              IN3 -> GPIO 26 (RIGHT)
 *              IN4 -> GND
 *              ENA/ENB jumpers ON
 *
 *   L298N #2:  IN1 -> GPIO 27 (BACK)
 *              IN2 -> GND
 *              IN3 -> GPIO 14 (LEFT)
 *              IN4 -> GND
 *              ENA/ENB jumpers ON
 */

#include <Arduino.h>

#define MOTOR_FRONT  25
#define MOTOR_RIGHT  26
#define MOTOR_BACK   27
#define MOTOR_LEFT   14

const int motors[] = {MOTOR_FRONT, MOTOR_RIGHT, MOTOR_BACK, MOTOR_LEFT};
const char* names[] = {"FRONT (GPIO 25)", "RIGHT (GPIO 26)", "BACK (GPIO 27)", "LEFT (GPIO 14)"};
const int NUM_MOTORS = 4;

void allOff() {
  for (int i = 0; i < NUM_MOTORS; i++) {
    digitalWrite(motors[i], LOW);
  }
}

void allOn() {
  for (int i = 0; i < NUM_MOTORS; i++) {
    digitalWrite(motors[i], HIGH);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println("   VIBRATOR MOTOR TEST");
  Serial.println("========================================\n");

  for (int i = 0; i < NUM_MOTORS; i++) {
    pinMode(motors[i], OUTPUT);
    digitalWrite(motors[i], LOW);
    Serial.printf("  %s - READY\n", names[i]);
  }

  Serial.println("\n--- Starting Test Sequence ---\n");

  // TEST 1: Each motor individually for 2 seconds
  for (int i = 0; i < NUM_MOTORS; i++) {
    Serial.printf(">> %s ON\n", names[i]);
    digitalWrite(motors[i], HIGH);
    delay(2000);
    digitalWrite(motors[i], LOW);
    Serial.printf("   %s OFF\n\n", names[i]);
    delay(500);
  }

  // TEST 2: All motors ON together
  Serial.println(">> ALL MOTORS ON");
  allOn();
  delay(3000);
  allOff();
  Serial.println("   ALL MOTORS OFF\n");

  Serial.println("========================================");
  Serial.println("   TEST COMPLETE");
  Serial.println("========================================");
  Serial.println("\nType in Serial Monitor:");
  Serial.println("  F = FRONT   R = RIGHT");
  Serial.println("  B = BACK    L = LEFT");
  Serial.println("  A = ALL ON  O = ALL OFF");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'f': case 'F': Serial.println("FRONT ON");  digitalWrite(MOTOR_FRONT, HIGH); break;
      case 'r': case 'R': Serial.println("RIGHT ON");  digitalWrite(MOTOR_RIGHT, HIGH); break;
      case 'b': case 'B': Serial.println("BACK ON");   digitalWrite(MOTOR_BACK, HIGH);  break;
      case 'l': case 'L': Serial.println("LEFT ON");   digitalWrite(MOTOR_LEFT, HIGH);  break;
      case 'a': case 'A': Serial.println("ALL ON");    allOn();  break;
      case 'o': case 'O': Serial.println("ALL OFF");   allOff(); break;
    }
  }
}
