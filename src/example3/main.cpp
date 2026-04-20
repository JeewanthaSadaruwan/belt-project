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

#define MAX_DATA_SIZE 240

#define LORA_RING_SIZE 4
struct LoRaRxEntry {
  char     data[MAX_DATA_SIZE];
  uint8_t  pktType;
  uint16_t pktCounter;
  int      rssi;
  bool     valid;
};
volatile LoRaRxEntry loraRing[LORA_RING_SIZE];
volatile int loraRingHead = 0;
volatile int loraRingTail = 0;

static void blinkLED(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

// NOTE: No IRAM_ATTR here — LoRa.read() / packetRssi() call SPI
// functions that live in flash. Placing this in IRAM causes a
// Guru Meditation (LoadProhibited) crash on every received packet.
// The rovar base station uses the same approach without IRAM_ATTR.
void onLoRaReceive(int packetSize) {
  if (packetSize < 4) return;

  int nextHead = (loraRingHead + 1) % LORA_RING_SIZE;
  if (nextHead == loraRingTail) return;

  volatile LoRaRxEntry* entry = &loraRing[loraRingHead];

  entry->rssi       = LoRa.packetRssi();
  entry->pktType    = LoRa.read();
  entry->pktCounter = ((uint16_t)LoRa.read() << 8) | LoRa.read();
  uint8_t dataLen   = LoRa.read();
  if (dataLen > MAX_DATA_SIZE - 1) dataLen = MAX_DATA_SIZE - 1;

  int idx = 0;
  while (LoRa.available() && idx < dataLen)
    entry->data[idx++] = (char)LoRa.read();
  entry->data[idx] = '\0';
  entry->valid = true;

  loraRingHead = nextHead;
}

bool initLoRa() {
  Serial.println("[LoRa] Starting LoRa receiver...");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  delay(100);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  delay(100);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] Init failed!");
    return false;
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x12);
  LoRa.enableCrc();

  LoRa.onReceive(onLoRaReceive);
  LoRa.receive();

  Serial.print("[LoRa] Frequency: ");
  Serial.print(LORA_FREQUENCY);
  Serial.println(" Hz");
  Serial.println("[LoRa] Init success! RX_CONTINUOUS active.");
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(1000);

  blinkLED(3, 100, 100);

  if (!initLoRa()) {
    while (1) blinkLED(1, 50, 50);
  }

  blinkLED(1, 600, 0);
}

void loop() {
  static unsigned long lastHB = 0;
  if (millis() - lastHB > 5000) {
    Serial.printf("[LoRa RX] Listening...  uptime=%lus  ring=%d/%d\n",
                  millis() / 1000, loraRingHead, LORA_RING_SIZE);
    lastHB = millis();
  }

  while (loraRingTail != loraRingHead) {
    volatile LoRaRxEntry* entry = &loraRing[loraRingTail];
    loraRingTail = (loraRingTail + 1) % LORA_RING_SIZE;
    if (!entry->valid) continue;

    blinkLED(2, 80, 80);

    Serial.println("\n════════════════════════════════════════");
    Serial.printf ("  PACKET RECEIVED  #%04u\n", entry->pktCounter);
    Serial.println("────────────────────────────────────────");
    Serial.printf ("  Type   : 0x%02X\n",  entry->pktType);
    Serial.printf ("  RSSI   : %d dBm\n",  entry->rssi);
    Serial.printf ("  Length : %d bytes\n",(int)strlen((char*)entry->data));
    Serial.printf ("  Data   : %s\n",      (char*)entry->data);
    Serial.println("════════════════════════════════════════\n");

    // Mark slot as consumed so it can be safely reused
    entry->valid = false;
  }

  delay(10);
}

