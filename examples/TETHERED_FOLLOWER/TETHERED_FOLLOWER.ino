#include <LEADER.h>

OSCFollower bridge2;

void setup() {
  // Channel 1, USB SLIP = true, Baud = 1000000
  bridge2.begin(1, true, 1000000); 
}

void loop() {
  // Silently routes USB to Radio and Radio to USB
  bridge2.update();
}