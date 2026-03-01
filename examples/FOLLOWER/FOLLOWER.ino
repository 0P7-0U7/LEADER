#include <LEADER.h>
#include <OSCMessage.h>

OSCFollower node;
const int LED_PIN = 2; // Standard ESP32 built-in LED

// 1. The function that runs when "/test" is received
void controlLED(OSCMessage &msg) {
  // Check if Pure Data sent a 1 or a 0
  if (msg.isInt(0)) {
    int state = msg.getInt(0);
    digitalWrite(LED_PIN, state > 0 ? HIGH : LOW);
  }
}

// 2. The callback that catches all incoming radio traffic
void onRadioData(const uint8_t *data, int len) {
  OSCMessage msg;
  msg.fill(const_cast<uint8_t *>(data),
           len); // Pour the raw radio array into CNMAT

  if (!msg.hasError()) {
    // If the address is "/test", trigger the controlLED function
    msg.dispatch("/test", controlLED);
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);

  // Channel 1, USB SLIP = false (Battery Mode)
  node.begin(1, false);
  node.enableHeartbeat(1000, 42);

  // Attach the listener
  node.onReceive(onRadioData);
}

void loop() {
  node.update();

  // Send a sensor reading every 50ms
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 50) {
    lastSend = millis();

    OSCMessage msg("/sensor/pot");
    msg.add((int32_t)analogRead(34));

    OSCBuffer buf;
    msg.send(buf);
    buf.end(); // Assemble and pad the array!

    node.send(buf.buffer, buf.length);
  }
}
