// ============================================================
//  EXAMPLE 4 — ESP32 MAC Address Reader
//  Prints the WiFi MAC address and BT MAC address on boot.
//  Useful for identifying each ESP32 board when setting up
//  ESP-NOW or LoRa peer-to-peer communication.
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  // ── WiFi MAC ─────────────────────────────────────────────
  // Initialize WiFi in station mode just to read the MAC.
  // Does NOT connect to any network.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  Serial.println("════════════════════════════════════════");
  Serial.println("  ESP32 MAC ADDRESS READER");
  Serial.println("════════════════════════════════════════");

  Serial.printf("  WiFi MAC (STA) : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // AP MAC is STA MAC + 1 on the last byte
  Serial.printf("  WiFi MAC (AP)  : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] + 1);

  // ── Bluetooth MAC ─────────────────────────────────────────
  // BT MAC = base MAC + 2
  Serial.printf("  BT   MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] + 2);

  Serial.println("────────────────────────────────────────");

  // ── Also print as byte array (for ESP-NOW peer registration)
  Serial.print("  As byte array  : {");
  for (int i = 0; i < 6; i++) {
    Serial.printf("0x%02X", mac[i]);
    if (i < 5) Serial.print(", ");
  }
  Serial.println("}");

  Serial.println("════════════════════════════════════════");
}

void loop() {
  // Nothing to do — MAC is printed once on boot
  delay(5000);
  Serial.println("[ready] Reset the board to read again.");
}
