#include <LEADER.h>

OSCLeader bridge;

void setup() {
  Serial.begin(1000000); 
  bridge.begin(Serial, 1000000, 1, false);
  bridge.setIndicator(2); // LED on pin 2 flashes on traffic
}

void loop() {
  bridge.update();
}