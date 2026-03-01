<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/LOGO_DARK.png">
  <source media="(prefers-color-scheme: light)" srcset="images/LOGO_LIGHT.png">
  <img alt="LEADER by OPT-OUT" src="images/LOGO_LIGHT.png" width="100%">
</picture>

# LEADER by OPT-OUT
**High-Performance OSC over ESP-NOW System**

A zero-latency, router-less wireless network bridge for live performance. Connects ESP32 nodes directly to Pure Data/MaxMSP via USB SLIP and high-speed radio.

### Features
* **Zero-Latency:** Bypasses standard Wi-Fi routers completely.
* **Tethered USB Bridging:** Flawless bidirectional communication between computers.
* **Frictionless Migration:** Fully compatible with standard CNMAT `OSCMessage`. Instantly port your old laggy UDP/Wi-Fi sketches over to ESP-NOW without rewriting your data structures.
* **MiniOSC Engine:** Background telemetry and automatic channel hopping keeps administrative traffic microscopic.

---

##  System Commands

| Address | Args | Description |
| :--- | :---: | :--- |
| `/leader/ping` | - | Returns telemetry: Channel, Uptime, Heap, Sent, Dropped. |
| `/leader/hop` | - | Leader forces network to find cleanest channel and migrate. |
| `/leader/nodes` | - | Requests Leader to transmit the registry of all active connected nodes. |
| `/sys/node` | int, int | Leader reply containing Follower Node ID and milliseconds since last seen. |
| `/sys/ping` | int | Sent from Leader. Sets heartbeat MS for all Followers (0 = OFF) |
| `/sys/pong` | int | Automatic Follower reply containing its unique node ID. |






## Quick Start Implementation

**ESP32 Hardware Compatibility:**
- **100% Variant Agnostic:** Native ESP-NOW architecture enables LEADER to run cleanly on all chips (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6).

### 1. Leader
Plugged into the master computer handling Pure Data. Translates SLIP OSC into high-speed radio broadcasts.
Its the DIRECTOR. it sends to every other device and receives from every other device, passing everything to your computer.
```cpp
#include <LEADER.h>
OSCLeader leader;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

void setup() {
  Serial.begin(230400); // enough is enough. pd[comport] limit seems to be
                        // 230400
  // Initialize the LEADER on Channel 1, autoHop = false
  leader.begin(Serial, 230400, 1, false);
  // Enable the built-in LED, blink for 40ms, using active-LOW (true for
  // XIAO...) blink when LEADER is sending data to FOLLOWERS
  leader.setIndicator(LED_BUILTIN, 40, true);
}

void loop() {
  // The library handles all the data, routing, and LED blinking internally!
  leader.update();

  // CIAO! :O)
}
```

### 2. Tethered Follower
Plugged into a secondary computer. Acts as a flawless two-way SLIP-to-Radio bridge.
```cpp
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
```

### 3. Standalone Sensor Follower
Standalone... not plugged to a computer (or plugged just for power) read sensors and/or play with actuators.

```cpp
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
```

---

### Full Documentation
For full architectural diagrams, API references, and Pure Data implementation guides, please download the library and open the included **`docs/index.html`** file in your browser.
OR JUST GO TO <a href="https://0p7-0u7.github.io/LEADER/" target="_blank">LEADER PAGE</a>
