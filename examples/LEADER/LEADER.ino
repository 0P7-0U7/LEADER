#include <LEADER.h>
OSCLeader leader;

void setup() {
  Serial.begin(230400); //enough is enough. pd[comport] limit seems to be 230400   
  // Initialize the LEADER on Channel 1, autoHop = false
  leader.begin(Serial, 230400, 1, false); 
  // Enable the built-in LED, blink for 40ms, using active-LOW (true for XIAO...)
  // blink when LEADER is sending data to FOLLOWERS
  leader.setIndicator(LED_BUILTIN, 40, true);
}

void loop() {
// The library handles all the data, routing, and LED blinking internally!
  leader.update();

  // CIAO! :O)

}
