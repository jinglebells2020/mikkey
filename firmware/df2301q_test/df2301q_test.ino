// Mikkey sanity test 2: make the DF2301QG play its canned phrases on command.
// Wiring: VCC->3V3, GND->GND, D/T->GPIO8 (SDA), C/R->GPIO9 (SCL), switch on I2C.
// Every 5s it plays the audio for the next command ID and prints which one.
// Still prints any command IDs it hears from you.

#include <Wire.h>
#include "DFRobot_DF2301Q.h"

DFRobot_DF2301Q_I2C ai;

const uint8_t demoIds[] = {1, 2, 5, 22, 23, 103};
uint8_t demoIdx = 0;
unsigned long nextPlay = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Mikkey DF2301QG speaker demo ===");

  Wire.begin(8, 9);
  while (!ai.begin()) {
    Serial.println("DF2301Q not found, check wiring/switch");
    delay(1000);
  }
  Serial.println("DF2301Q OK");
  ai.setVolume(7);
  ai.setMuteMode(0);
  ai.setWakeTime(15);
  nextPlay = millis() + 2000;
}

void loop() {
  uint8_t heard = ai.getCMDID();
  if (heard != 0) {
    Serial.print("heard command id: ");
    Serial.println(heard);
  }
  delay(50);
}
