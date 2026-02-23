#include <LEADER.h>

OSCFollower theothercomputer;

void setup() {
  // Channel 1, USB SLIP = true, Baud = 1000000 is max...
  theothercomputer.begin(1, true, 230400); 
}

void loop() {
  // Silently routes USB to Radio and Radio to USB
  theothercomputer.update();

  // CIAO :O)
}
