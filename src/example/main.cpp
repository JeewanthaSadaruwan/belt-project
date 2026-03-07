/*
 * MAC ADDRESS READER
 * Simple utility to read and display ESP32 MAC address
 * Use this to get your Base Station's MAC address
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║                                                            ║");
  Serial.println("║              ESP32 MAC ADDRESS READER                     ║");
  Serial.println("║                                                            ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");
  Serial.println();
  
  // Get MAC address in Station mode
  WiFi.mode(WIFI_STA);
  uint8_t macSTA[6];
  WiFi.macAddress(macSTA);
  
  Serial.println("📍 STATION MODE MAC ADDRESS:");
  Serial.println("   ═════════════════════════════════════");
  Serial.print("   ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", macSTA[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  
  Serial.println("\n   For Belt 2 code, use this format:");
  Serial.print("   uint8_t baseStationMAC[] = {");
  for (int i = 0; i < 6; i++) {
    Serial.printf("0x%02X", macSTA[i]);
    if (i < 5) Serial.print(", ");
  }
  Serial.println("};");
  
  // Get MAC address in AP mode
  WiFi.mode(WIFI_AP);
  delay(100);
  uint8_t macAP[6];
  esp_wifi_get_mac(WIFI_IF_AP, macAP);
  
  Serial.println("\n\n📡 ACCESS POINT MODE MAC ADDRESS:");
  Serial.println("   ═════════════════════════════════════");
  Serial.print("   ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", macAP[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  
  Serial.println("\n   For Belt 2 code, use this format:");
  Serial.print("   uint8_t baseStationMAC[] = {");
  for (int i = 0; i < 6; i++) {
    Serial.printf("0x%02X", macAP[i]);
    if (i < 5) Serial.print(", ");
  }
  Serial.println("};");
  
  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║  INSTRUCTIONS:                                             ║");
  Serial.println("║                                                            ║");
  Serial.println("║  1. Upload this code to your Base Station 2 ESP32         ║");
  Serial.println("║  2. Copy the AP MODE MAC ADDRESS shown above              ║");
  Serial.println("║  3. Update Belt 2 code with that MAC address              ║");
  Serial.println("║  4. Then upload the regular Base Station 2 firmware       ║");
  Serial.println("║                                                            ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");
  Serial.println();
  
  Serial.println("✅ Done! You can now close the serial monitor.");
  Serial.println();
}

void loop() {
  // Nothing to do here
  delay(1000);
}
