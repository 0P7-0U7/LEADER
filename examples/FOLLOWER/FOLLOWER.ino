#include <LEADER.h>
#include <OSCMessage.h>

OSCFollower node;

void setup() {
  // Channel 1, USB SLIP = false (Battery Mode)
  node.begin(1, false); 
  node.enableHeartbeat(1000, 42); // Send ID 42 every 1 sec
}

void loop() {
  node.update();

  // Send a sensor reading every 50ms
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 50) {
    lastSend = millis();
    
    OSCMessage msg("/sensor/pot");
    msg.add(analogRead(34));

    OSCBuffer buf;
    msg.send(buf);
    buf.end(); // Assemble and pad the array!
    
    node.send(buf.buffer, buf.length);
  }
}