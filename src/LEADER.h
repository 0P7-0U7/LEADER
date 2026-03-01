#ifndef LEADER_H
#define LEADER_H

#include <Arduino.h>
#include <Stream.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

typedef void (*OSCReceiveCallback)(const uint8_t *data, int len);

// ==========================================
// The Universal CNMAT Adaptor Bucket
// ==========================================

/**
 * @brief Universal CNMAT Adaptor Bucket for Serial to OSC payload alignment.
 *
 * Provides a custom abstraction inheriting from Print. This allows standard
 * CNMAT OSCMessage objects to send data transparently, buffering everything
 * and padding nicely on 4-byte boundaries at the end to satisfy Pure Data
 * requirements.
 */
class OSCBuffer : public Print {
public:
  uint8_t
      buffer[250]; ///< Maximum payload size allowed for esp_now is 250 bytes
  size_t length = 0;

  /**
   * @brief Intercepts single byte writes.
   * @param b The byte to write.
   * @return Number of bytes written (1 on success, 0 on overflow).
   */
  size_t write(uint8_t b) override {
    if (length < sizeof(buffer)) {
      buffer[length++] = b;
      return 1;
    }
    return 0; // Prevent buffer overflow gracefully
  }

  /**
   * @brief Intercepts bulk byte writes representing higher-level CNMAT
   * structures.
   * @param str The payload to add.
   * @param size Length of the payload.
   * @return The number of bytes successfully written.
   */
  size_t write(const uint8_t *str, size_t size) override {
    size_t count = 0;
    while (size--) {
      if (write(*str++))
        count++;
      else
        break;
    }
    return count;
  }

  /**
   * @brief Ensures that the underlying data length matches OSC 4-byte padding
   * rules.
   *
   * Appends null terminators until the valid payload rests perfectly on a
   * 4-byte boundary. Hardened to avoid infinite looping if the buffer hits
   * maximum capacity.
   */
  void end() {
    // Bounded padding to prevent infinite loops where length is at the max
    // capacity
    while (length % 4 != 0 && length < sizeof(buffer)) {
      write(0);
    }
  }

  /**
   * @brief Empties the internal buffer.
   */
  void clear() { length = 0; }
};

// ==========================================
// System Network Object Directors
// ==========================================

/**
 * @brief High-speed OSC/SLIP-over-Radio Director Node.
 *
 * Designed for the primary central machine (usually running Pure Data/MaxMSP).
 * Accepts SLIP formatted OSC data over serial, translating it to ESP-NOW
 * broadcast architecture. Intercepts local telemetry checks without network
 * congestion.
 */
class OSCLeader {
public:
  /**
   * @brief Initializes the Leader node network state, Wi-Fi parameters, and
   * serial mapping.
   *
   * @param serialPort Reference to the Serial stream connected to the host
   * computer.
   * @param baudRate Connection speed (recommended 1,000,000 to maximize
   * throughput).
   * @param homeChannel Baseline Wi-Fi channel for initial sync (1-13).
   * @param autoHop Flag requesting dynamic channel scanning to prevent
   * interference.
   */
  void begin(Stream &serialPort, long baudRate = 1000000,
             uint8_t homeChannel = 1, bool autoHop = false);

  /**
   * @brief Ongoing process to handle Serial incoming payloads and emit network
   * events.
   *
   * Requires placement inside the main loop() function repeatedly to keep the
   * node active.
   *
   * @return True if a packet was successfully forwarded to radio.
   */
  bool update();

  /**
   * @brief Configures blink activity during outgoing network traffic as a
   * visual monitor.
   *
   * @param pin The GPIO index dedicated to an indicator LED (e.g. LED_BUILTIN).
   * @param blinkDuration Milliseconds the LED remains lit per packet sent.
   * @param activeLow Hardware logic behavior, set to true if LED sinks current.
   */
  void setIndicator(int pin, unsigned long blinkDuration = 20,
                    bool activeLow = true);

  /**
   * @brief Transmits the current node registry to the Host Computer.
   */
  void sendNodeRegistry();

private:
  Stream *_serial;
  uint8_t _homeChannel;
  uint8_t _broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t _peerInfo;

  uint8_t _oscBuffer[250];
  uint16_t _bufferIndex = 0;
  bool _isEscaping = false;

  // Built-in LED Variables
  int _ledPin = -1;
  unsigned long _blinkDuration = 20;
  unsigned long _lastDataTime = 0;
  bool _isLedOn = false;
  uint8_t _ledOnState = LOW;
  uint8_t _ledOffState = HIGH;

  // --- Telemetry Counters ---
  uint32_t _packetsSent = 0;
  uint32_t _packetsDropped = 0;

// --- Node Registry ---
#define MAX_NODES 32
  struct NodeRecord {
    uint8_t mac[6];
    uint32_t nodeID;
    unsigned long lastSeen;
    bool active;
  };
  NodeRecord _activeNodes[MAX_NODES];
  uint8_t _nodeCount = 0;
  void updateNodeRegistry(const uint8_t *mac, uint32_t nodeID);

  /**
   * @brief Internal logic scanning existing Wi-Fi APs to evaluate congestion
   * density.
   * @return The channel numbering from 1-13 with lowest interference load.
   */
  uint8_t findQuietestChannel();

  /**
   * @brief Assembles an OSC string "/leader/channel" and reports back to Host
   * Computer.
   */
  void sendChannelFeedback();

  /**
   * @brief Compiles hardware telemetry arguments and beams it over SLIP to
   * Host.
   */
  void sendPingReply();

  /**
   * @brief Discovers strongest Wi-Fi channel and forcefully reassigns entire
   * mesh architecture.
   */
  void triggerHop();

  static OSCLeader *_instance;
  static void _staticOnDataRecv(const esp_now_recv_info_t *info,
                                const uint8_t *incomingData, int len);
  void _handleDataRecv(const uint8_t *mac, const uint8_t *incomingData,
                       int len);
};

/**
 * @brief Endpoint Client Node mapping internal logic or external hardware to
 * central Leader.
 *
 * Functions autonomously via battery reading logic gates or tethered
 * translating parallel host machines into radio broadcasts identical to Leader.
 */
class OSCFollower {
public:
  /**
   * @brief Bootstraps internal framework binding Follower node to incoming
   * channels.
   *
   * @param homeChannel Fixed channel identifier to await incoming
   * transmissions.
   * @param enableUSB Enables explicit bridging forwarding ESP-NOW packets to
   * SLIP computer output.
   * @param baudRate Connection speed necessary if enableUSB is flagged true.
   */
  void begin(uint8_t homeChannel = 1, bool enableUSB = false,
             long baudRate = 115200);

  /**
   * @brief Required ongoing cycle process listening for SLIP/ESP-NOW exchanges
   * over the internal hardware layer and executing callbacks.
   */
  void update();

  /**
   * @brief Subscribes a custom user callback routine invoked for raw network
   * data.
   *
   * @param callback Void routine receiving pointer to raw packet payload and
   * size offset.
   */
  void onReceive(OSCReceiveCallback callback);

  /**
   * @brief Transmits unstructured binary payload out through wireless
   * architecture bound for the cached Leader identity.
   *
   * @param data Byte pointer corresponding to memory offset.
   * @param len Quantity of valid bytes encoded.
   */
  void send(const uint8_t *data, int len);

  /**
   * @brief Initiates scheduled system polling broadcasting unique identity
   * codes automatically.
   *
   * @param interval Millisecond delay loop gap between repeated broadcasts.
   * @param customID Internal Node Identity code offset (Generates random
   * default if zero).
   */
  void enableHeartbeat(uint32_t interval, uint32_t customID = 0);

private:
  static OSCFollower *_instance;
  static void _staticOnDataRecv(const esp_now_recv_info_t *info,
                                const uint8_t *incomingData, int len);
  void _handleDataRecv(const uint8_t *mac, const uint8_t *incomingData,
                       int len);

  uint8_t _homeChannel;
  uint8_t _currentChannel;
  uint8_t _leaderMac[6];
  bool _leaderMacSet = false;
  unsigned long _lastMessageTime;
  OSCReceiveCallback _userCallback = nullptr;

  // Heartbeat memory variables
  uint32_t _nodeID;
  uint32_t _heartbeatInterval = 0;
  unsigned long _lastHeartbeatTime = 0;
  bool _heartbeatEnabled = false;

  // SLIP USB Variables for Tethered Mode
  bool _usbEnabled = false;
  uint8_t _serialRxBuf[512];
  int _serialRxLen = 0;
  bool _serialEscaping = false;

  void _handleSerial();
  void _sendSlipToUSB(const uint8_t *data, int len);
};

#endif