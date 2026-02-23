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

## Quick Start Implementation

### 1. The Leader Bridge
Plugged into the master computer handling Pure Data. Translates SLIP OSC into high-speed radio broadcasts.
```cpp
#include <LEADER.h>

OSCLeader bridge;

void setup() {
  Serial.begin(1000000); 
  // Start on Channel 1, auto-hopping disabled
  bridge.begin(Serial, 1000000, 1, false);
  bridge.setIndicator(2); // LED on pin 2 flashes on traffic
}

void loop() {
  bridge.update();
}
```

### 2. Tethered Follower
Plugged into a secondary computer (Visuals/MaxMSP). Acts as a flawless two-way SLIP-to-Radio bridge.
```cpp
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
```

### 3. Standalone Sensor Follower
Running on a battery, reading a sensor, and correctly assembling the array for ESP-NOW using the built-in CNMAT adaptor.
```cpp
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
```

---

### Full Documentation
For full architectural diagrams, API references, and Pure Data implementation guides, please download the library and open the included **`docs.html`** file in your browser.
