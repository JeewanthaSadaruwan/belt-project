#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

#define LORA_CS    5
#define LORA_RST   14
#define LORA_DIO0  26
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LED_PIN    2

#ifndef LORA_FREQUENCY
#define LORA_FREQUENCY 433E6
#endif

#define MAX_DATA_SIZE 220
#define PKT_DATA 0x10

static uint16_t loraPacketCounter = 0;
static const unsigned long TX_INTERVAL_MS = 1000;
static unsigned long last_tx_ms = 0;

static void blinkLED(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

static bool initLoRa() {
  Serial.println("[LoRa] Starting LoRa transmitter...");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] Init failed!");
    return false;
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();
  LoRa.setTxPower(20);

  Serial.print("[LoRa] Frequency: ");
  Serial.print(LORA_FREQUENCY);
  Serial.println(" Hz");
  Serial.println("[LoRa] Init success!");
  return true;
}

static bool sendLoRaPacketRaw(uint8_t type, const char *data) {
  uint8_t buffer[4 + MAX_DATA_SIZE];
  size_t dataLen = strlen(data);
  if (dataLen > MAX_DATA_SIZE) dataLen = MAX_DATA_SIZE;

  buffer[0] = type;
  buffer[1] = (uint8_t)(loraPacketCounter >> 8);
  buffer[2] = (uint8_t)(loraPacketCounter & 0xFF);
  buffer[3] = (uint8_t)dataLen;
  memcpy(&buffer[4], data, dataLen);

  LoRa.beginPacket();
  LoRa.write(buffer, 4 + dataLen);
  int result = LoRa.endPacket();

  if (result) loraPacketCounter++;
  return result != 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(1000);
  Serial.println("BOOT");

  blinkLED(3, 100, 100); // booting

  if (!initLoRa()) {
    while (1) blinkLED(1, 50, 50); // init failed
  }

  blinkLED(1, 600, 0); // init success

  last_tx_ms = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - last_tx_ms < TX_INTERVAL_MS) return;

  char payload[128];
  snprintf(payload, sizeof(payload),
           "hdg=0.0,bear=63.3,err=63.3,dist=519.1,"
           "ax=0.00,ay=0.00,az=0.00,gz=0.00,t=0.0,dir=TURN_RIGHT");

  bool ok = sendLoRaPacketRaw(PKT_DATA, payload);

  Serial.println("\n────────────────────────────────────────");
  if (ok) {
    blinkLED(1, 80, 0);
    Serial.printf("[LoRa TX] #%04u  OK\n", loraPacketCounter - 1);
  } else {
    blinkLED(3, 80, 80);
    Serial.printf("[LoRa TX] #%04u  FAIL\n", loraPacketCounter);
  }
  Serial.printf("  Type   : 0x%02X\n", PKT_DATA);
  Serial.printf("  Freq   : %.0f Hz\n", (float)LORA_FREQUENCY);
  Serial.printf("  Payload: %s\n", payload);
  Serial.printf("  Length : %d bytes\n", (int)strlen(payload));
  Serial.println("────────────────────────────────────────");

  last_tx_ms = now;
}