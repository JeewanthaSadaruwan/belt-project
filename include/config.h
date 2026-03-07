#ifndef CONFIG_H
#define CONFIG_H

// ==================== WIFI CONFIGURATION ====================
// Change these to match your WiFi network
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ==================== LORA CONFIGURATION ====================
#define LORA_FREQUENCY 433E6  // 433 MHz (or 868E6 for 868 MHz, 915E6 for 915 MHz)
#define LORA_SPREADING_FACTOR 12
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE 5
#define LORA_SYNC_WORD 0x12

// ==================== TIMING CONFIGURATION ====================
#define DATA_SEND_INTERVAL 2000  // milliseconds between data transmissions

// ==================== GPS CONFIGURATION ====================
#define GPS_BAUD_RATE 9600

#endif // CONFIG_H
