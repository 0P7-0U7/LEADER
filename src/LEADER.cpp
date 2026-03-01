#include "LEADER.h"
#include "MiniOSC.h"
#include <esp_mac.h>

// ==========================================
// LEADER IMPLEMENTATION
// ==========================================

OSCLeader *OSCLeader::_instance = nullptr;

void OSCLeader::begin(Stream &serialPort, long baudRate, uint8_t homeChannel,
                      bool autoHop) {
  _instance = this;
  _serial = &serialPort;
  _homeChannel = homeChannel;

  // Configure Wi-Fi in Station Mode and disable power saving for lowest latency
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK)
    return;

  // Register the global static receive callback mapped to the singleton
  // instance
  esp_now_register_recv_cb(_staticOnDataRecv);

  // Bind to the designated home Wi-Fi channel
  esp_wifi_set_channel(_homeChannel, WIFI_SECOND_CHAN_NONE);

  // Register the broadcast address as a unified, channel-wide peer
  memcpy(_peerInfo.peer_addr, _broadcastAddress, 6);
  _peerInfo.channel = _homeChannel;
  _peerInfo.encrypt = false; // Security tradeoff for maximum throughput speed
  esp_now_add_peer(&_peerInfo);
}

void OSCLeader::setIndicator(int pin, unsigned long blinkDuration,
                             bool activeLow) {
  _ledPin = pin;
  _blinkDuration = blinkDuration;
  _ledOnState = activeLow ? LOW : HIGH;
  _ledOffState = activeLow ? HIGH : LOW;

  if (_ledPin >= 0) {
    pinMode(_ledPin, OUTPUT);
    digitalWrite(_ledPin, _ledOffState);
  }
}

void OSCLeader::triggerHop() {
  uint8_t targetChannel = findQuietestChannel();

  // Create a raw 4-byte explicit hopping command (0xFE 0xFE 0xFE [CHANNEL])
  uint8_t hopMessage[4] = {0xFE, 0xFE, 0xFE, targetChannel};

  // Blast the hop command aggressively to ensure followers receive it before
  // migration
  for (int i = 0; i < 10; i++) {
    esp_now_send(_broadcastAddress, hopMessage, sizeof(hopMessage));
    delay(10);
  }

  // Migrate Leader to the newly designated channel
  esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
  _peerInfo.channel = targetChannel;
  esp_now_mod_peer(&_peerInfo);

  // Notify the host application of the successful migration
  sendChannelFeedback();
}

void OSCLeader::sendChannelFeedback() {
  OSCValue outVal;
  outVal.type = 'i';
  outVal.i = _peerInfo.channel;

  uint8_t outBuffer[64];
  int outLen = MiniOSC::pack(outBuffer, "/leader/channel", &outVal, 1);

  uint8_t slipBuffer[128];
  int slipIndex = 0;

  // SLIP Encoding (Standard RFC 1055)
  slipBuffer[slipIndex++] = 0xC0; // END
  for (int i = 0; i < outLen; i++) {
    if (outBuffer[i] == 0xC0) {
      slipBuffer[slipIndex++] = 0xDB; // ESC
      slipBuffer[slipIndex++] = 0xDC; // ESC_END
    } else if (outBuffer[i] == 0xDB) {
      slipBuffer[slipIndex++] = 0xDB; // ESC
      slipBuffer[slipIndex++] = 0xDD; // ESC_ESC
    } else {
      slipBuffer[slipIndex++] = outBuffer[i];
    }
  }
  slipBuffer[slipIndex++] = 0xC0; // END

  _serial->write(slipBuffer, slipIndex);
  _serial->flush();
}

void OSCLeader::sendPingReply() {
  OSCValue outVals[5];

  // Compile global node telemetry tracking states
  outVals[0].type = 'i';
  outVals[0].i = _peerInfo.channel;
  outVals[1].type = 'i';
  outVals[1].i = millis() / 1000;
  outVals[2].type = 'i';
  outVals[2].i = ESP.getFreeHeap();
  outVals[3].type = 'i';
  outVals[3].i = _packetsSent;
  outVals[4].type = 'i';
  outVals[4].i = _packetsDropped;

  uint8_t outBuffer[128];
  int outLen = MiniOSC::pack(outBuffer, "/leader/ping", outVals, 5);

  uint8_t slipBuffer[256];
  int slipIndex = 0;

  // SLIP Encoding wrapper
  slipBuffer[slipIndex++] = 0xC0;
  for (int i = 0; i < outLen; i++) {
    if (outBuffer[i] == 0xC0) {
      slipBuffer[slipIndex++] = 0xDB;
      slipBuffer[slipIndex++] = 0xDC;
    } else if (outBuffer[i] == 0xDB) {
      slipBuffer[slipIndex++] = 0xDB;
      slipBuffer[slipIndex++] = 0xDD;
    } else {
      slipBuffer[slipIndex++] = outBuffer[i];
    }
  }
  slipBuffer[slipIndex++] = 0xC0;

  _serial->write(slipBuffer, slipIndex);
  _serial->flush();
}

uint8_t OSCLeader::findQuietestChannel() {
  int numNetworks = WiFi.scanNetworks();
  int channelCounts[14] = {0};

  // Poll physical wireless environment enumerating active ESSIDs mapped to
  // channels
  for (int i = 0; i < numNetworks; i++) {
    uint8_t ch = WiFi.channel(i);
    if (ch >= 1 && ch <= 13)
      channelCounts[ch]++;
  }

  uint8_t bestChannel = 1;
  int lowestCount = 999;

  for (uint8_t i = 1; i <= 13; i++) {
    if (channelCounts[i] < lowestCount) {
      lowestCount = channelCounts[i];
      bestChannel = i;
    }
  }

  WiFi.scanDelete();
  return bestChannel;
}

void OSCLeader::updateNodeRegistry(const uint8_t *mac, uint32_t nodeID) {
  // Check if node already exists
  for (int i = 0; i < _nodeCount; i++) {
    if (memcmp(_activeNodes[i].mac, mac, 6) == 0) {
      _activeNodes[i].nodeID = nodeID;
      _activeNodes[i].lastSeen = millis();
      _activeNodes[i].active = true;
      return;
    }
  }

  // Create new node if there is space
  if (_nodeCount < MAX_NODES) {
    memcpy(_activeNodes[_nodeCount].mac, mac, 6);
    _activeNodes[_nodeCount].nodeID = nodeID;
    _activeNodes[_nodeCount].lastSeen = millis();
    _activeNodes[_nodeCount].active = true;
    _nodeCount++;
  }
}

void OSCLeader::sendNodeRegistry() {
  unsigned long currentMillis = millis();

  for (int i = 0; i < _nodeCount; i++) {
    // If node hasn't been seen in 10 seconds, mark it inactive
    if (currentMillis - _activeNodes[i].lastSeen > 10000) {
      _activeNodes[i].active = false;
      continue; // Skip sending inactive nodes
    }

    OSCValue outVals[2];
    outVals[0].type = 'i';
    outVals[0].i = _activeNodes[i].nodeID;
    outVals[1].type = 'i';
    outVals[1].i = currentMillis - _activeNodes[i].lastSeen;

    uint8_t outBuffer[128];
    int outLen = MiniOSC::pack(outBuffer, "/sys/node", outVals, 2);

    uint8_t slipBuffer[256];
    int slipIndex = 0;

    slipBuffer[slipIndex++] = 0xC0;
    for (int j = 0; j < outLen; j++) {
      if (outBuffer[j] == 0xC0) {
        slipBuffer[slipIndex++] = 0xDB;
        slipBuffer[slipIndex++] = 0xDC;
      } else if (outBuffer[j] == 0xDB) {
        slipBuffer[slipIndex++] = 0xDB;
        slipBuffer[slipIndex++] = 0xDD;
      } else {
        slipBuffer[slipIndex++] = outBuffer[j];
      }
    }
    slipBuffer[slipIndex++] = 0xC0;

    _serial->write(slipBuffer, slipIndex);
  }
  _serial->flush();
}

bool OSCLeader::update() {
  // Handle asynchronous status LED reset cycle
  if (_ledPin >= 0 && _isLedOn && (millis() - _lastDataTime > _blinkDuration)) {
    digitalWrite(_ledPin, _ledOffState);
    _isLedOn = false;
  }

  bool actionTriggered = false;

  // Read SLIP-encoded OSC payloads incoming sequentially from Host Computer
  while (_serial->available()) {
    uint8_t incomingByte = _serial->read();

    if (incomingByte == 0xC0) { // SLIP END character
      if (_bufferIndex > 0) {

        // Intercept local telemetry ping address natively
        if (strncmp((const char *)_oscBuffer, "/leader/ping", 12) == 0 &&
            _oscBuffer[12] == '\0') {
          sendPingReply();
          actionTriggered = true;
        }
        // Intercept forcing manual channel hopping mechanism
        else if (strncmp((const char *)_oscBuffer, "/leader/hop", 11) == 0 &&
                 _oscBuffer[11] == '\0') {
          triggerHop();
          actionTriggered = true;
        }
        // Intercept node registry query
        else if (strncmp((const char *)_oscBuffer, "/leader/nodes", 13) == 0 &&
                 _oscBuffer[13] == '\0') {
          sendNodeRegistry();
          actionTriggered = true;
        }
        // Forward standard commands transparently out to the radio architecture
        else {
          esp_err_t result =
              esp_now_send(_broadcastAddress, _oscBuffer, _bufferIndex);
          if (result == ESP_OK) {
            _packetsSent++;
          } else {
            // Note: Retrying dropped packets breaks zero-latency constraint,
            // therefore drops are counted dynamically but not continuously
            // re-transmitted.
            _packetsDropped++;
          }
          actionTriggered = true;
        }

        _bufferIndex = 0; // Reset payload cache for subsequent chunks
      }
    } else if (incomingByte == 0xDB) { // SLIP ESC character
      _isEscaping = true;
    } else {
      // Decode escaped specialized bytes inline smoothly
      if (_isEscaping) {
        if (incomingByte == 0xDC)
          incomingByte = 0xC0;
        if (incomingByte == 0xDD)
          incomingByte = 0xDB;
        _isEscaping = false;
      }
      // Populate memory tracking cache against payload limits gracefully
      if (_bufferIndex < sizeof(_oscBuffer)) {
        _oscBuffer[_bufferIndex++] = incomingByte;
      } else {
        _bufferIndex =
            0; // Force empty and abandon malformed oversized transmissions
      }
    }
  }

  // Update UI logic indicators mapped visually via user request parameters
  if (actionTriggered && _ledPin >= 0) {
    digitalWrite(_ledPin, _ledOnState);
    _lastDataTime = millis();
    _isLedOn = true;
  }

  return actionTriggered;
}

void OSCLeader::_staticOnDataRecv(const esp_now_recv_info_t *info,
                                  const uint8_t *incomingData, int len) {
  if (_instance)
    _instance->_handleDataRecv(info->src_addr, incomingData, len);
}

void OSCLeader::_handleDataRecv(const uint8_t *mac, const uint8_t *incomingData,
                                int len) {
  // Update Node Registry on any incoming message
  // If it's a pong, extract the ID, otherwise use a default ID (0) to track its
  // MAC at least.
  uint32_t possibleNodeID = 0;
  if (len > 9 && strncmp((const char *)incomingData, "/sys/pong", 9) == 0) {
    OSCValue pongArgs[1];
    int pongCount =
        MiniOSC::extract(incomingData, len, "/sys/pong", pongArgs, 1);
    if (pongCount > 0 && pongArgs[0].type == 'i') {
      possibleNodeID = pongArgs[0].i;
    }
  }
  updateNodeRegistry(mac, possibleNodeID);

  uint8_t slipBuffer[512]; // Safeguard maximum SLIP size expansion
  int slipIndex = 0;

  // Embed radio segment back into standard SLIP mapping protocols inline
  slipBuffer[slipIndex++] = 0xC0;
  for (int i = 0; i < len; i++) {
    if (incomingData[i] == 0xC0) {
      slipBuffer[slipIndex++] = 0xDB;
      slipBuffer[slipIndex++] = 0xDC;
    } else if (incomingData[i] == 0xDB) {
      slipBuffer[slipIndex++] = 0xDB;
      slipBuffer[slipIndex++] = 0xDD;
    } else {
      slipBuffer[slipIndex++] = incomingData[i];
    }
  }
  slipBuffer[slipIndex++] = 0xC0;

  _serial->write(slipBuffer, slipIndex);
  // 2. NO FLUSH: Removed _serial->flush() here because it
  // causes packet fragmentation on newer ESP32 Native USB cores.
}

// ==========================================
// FOLLOWER IMPLEMENTATION
// ==========================================

OSCFollower *OSCFollower::_instance = nullptr;

void OSCFollower::begin(uint8_t homeChannel, bool enableUSB, long baudRate) {
  _instance = this;
  _homeChannel = homeChannel;
  _currentChannel = homeChannel;
  _usbEnabled = enableUSB;

  // Initialize tethered bridging state bindings conditionally
  if (_usbEnabled) {
    Serial.begin(baudRate);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(_homeChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK)
    return;

  esp_now_register_recv_cb(_staticOnDataRecv);
  _lastMessageTime = millis();

  // Generate a distinct internal tracking 16-bit identifier dynamically based
  // on MAC endcaps
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  _nodeID = (mac[4] << 8) | mac[5];
}

void OSCFollower::onReceive(OSCReceiveCallback callback) {
  _userCallback = callback;
}

void OSCFollower::send(const uint8_t *data, int len) {
  if (_leaderMacSet)
    esp_now_send(_leaderMac, data, len);
}

void OSCFollower::enableHeartbeat(uint32_t interval, uint32_t customID) {
  _heartbeatInterval = interval;
  if (customID != 0) {
    _nodeID = customID;
  }
  _heartbeatEnabled = true;

  // Added safety heartbeat default MAC configuration logic could map here
}

void OSCFollower::_staticOnDataRecv(const esp_now_recv_info_t *info,
                                    const uint8_t *incomingData, int len) {
  if (_instance)
    _instance->_handleDataRecv(info->src_addr, incomingData, len);
}

void OSCFollower::_handleDataRecv(const uint8_t *mac,
                                  const uint8_t *incomingData, int len) {
  _lastMessageTime = millis();

  // Perform a sanity check before locking onto a presumed Leader node MAC to
  // prevent pairing bugs
  if (!_leaderMacSet) {
    bool isValidLeaderPacket = false;

    // Valid packet verification: Looks for an explicit 0xFE structural hopping
    // phrase OR a standard string OSC identifier
    if (len == 4 && incomingData[0] == 0xFE && incomingData[1] == 0xFE &&
        incomingData[2] == 0xFE) {
      isValidLeaderPacket = true;
    } else if (len > 0 && incomingData[0] == '/') {
      isValidLeaderPacket = true;
    }

    if (isValidLeaderPacket) {
      memcpy(_leaderMac, mac, 6);
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, _leaderMac, 6);
      peerInfo.channel = _currentChannel;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
      _leaderMacSet = true;
    }
  }

  // Check 1: Hidden Hardware Hop Command
  if (len == 4 && incomingData[0] == 0xFE && incomingData[1] == 0xFE &&
      incomingData[2] == 0xFE) {
    uint8_t targetChannel = incomingData[3];
    if (targetChannel != _currentChannel) {
      _currentChannel = targetChannel;
      esp_wifi_set_channel(_currentChannel, WIFI_SECOND_CHAN_NONE);
      if (_leaderMacSet) {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, _leaderMac, 6);
        peerInfo.channel = _currentChannel;
        esp_now_mod_peer(&peerInfo);
      }
    }
    return; // Exit directly without processing payload downstream into user
            // loop parameters
  }

  // Intercept nested node enumeration systems globally via transparent system
  // callbacks Quick bounds testing comparing OSC initial string headers
  // explicitly
  if (len > 9 && strncmp((const char *)incomingData, "/sys/ping", 9) == 0) {
    OSCValue pingCheck[1];
    int pingArgs =
        MiniOSC::extract(incomingData, len, "/sys/ping", pingCheck, 1);

    // Scenario A: Host transmits "/sys/ping 40" dynamically shifting refresh
    // rate gap
    if (pingArgs > 0 && pingCheck[0].type == 'i') {
      int interval = pingCheck[0].i;
      if (interval > 0) {
        _heartbeatInterval = interval;
        _heartbeatEnabled = true;
      } else {
        _heartbeatEnabled = false;
      }
    }
    // Scenario B: Global host triggers a single polling snapshot sequence
    // without variables
    else if (pingArgs == 0) {
      OSCValue outVal;
      outVal.type = 'i';
      outVal.i = _nodeID;
      uint8_t outBuffer[64];
      int outLen = MiniOSC::pack(outBuffer, "/sys/pong", &outVal, 1);
      send(outBuffer, outLen);
    }
  }

  // Dispatch raw pointer parameters passing handling responsibility down into
  // user sketch layer implementation
  if (_userCallback)
    _userCallback(incomingData, len);

  // Cross-route ESP-NOW data back out through hardware SLIP protocols into
  // bridging Host Computer sequentially
  if (_usbEnabled) {
    _sendSlipToUSB(incomingData, len);
  }
}

void OSCFollower::update() {
  // Pass Serial polling into active background processing tracking 0xC0 SLIP
  // breakpoints
  if (_usbEnabled) {
    _handleSerial();
  }

  if (_heartbeatEnabled && _leaderMacSet) {
    if (millis() - _lastHeartbeatTime >= _heartbeatInterval) {
      _lastHeartbeatTime = millis();

      OSCValue outVal;
      outVal.type = 'i';
      outVal.i = _nodeID;

      uint8_t outBuffer[64];
      int outLen = MiniOSC::pack(outBuffer, "/sys/pong", &outVal, 1);

      send(outBuffer, outLen);
    }
  }
}

// ==========================================
// FOLLOWER SLIP USB ENGINES
// ==========================================

void OSCFollower::_sendSlipToUSB(const uint8_t *data, int len) {
  // OSC Standard alignment mandates valid structures to be strict multiples of
  // 4 bytes
  if (len == 0 || len % 4 != 0)
    return;

  uint8_t slipBuffer[512];
  int slipIndex = 0;

  slipBuffer[slipIndex++] = 0xC0;
  for (int i = 0; i < len; i++) {
    if (data[i] == 0xC0) {
      slipBuffer[slipIndex++] = 0xDB;
      slipBuffer[slipIndex++] = 0xDC;
    } else if (data[i] == 0xDB) {
      slipBuffer[slipIndex++] = 0xDB;
      slipBuffer[slipIndex++] = 0xDD;
    } else {
      slipBuffer[slipIndex++] = data[i];
    }
  }
  slipBuffer[slipIndex++] = 0xC0;

  Serial.write(slipBuffer, slipIndex);
}

void OSCFollower::_handleSerial() {
  while (Serial.available()) {
    uint8_t c = Serial.read();

    if (c == 0xC0) {
      if (_serialRxLen > 0) {
        // Successful isolated segment captured intact; bridge immediately to
        // Leader MAC node transparently
        if (_leaderMacSet) {
          esp_now_send(_leaderMac, _serialRxBuf, _serialRxLen);
        }
        _serialRxLen = 0;
      }
      _serialEscaping = false;
    } else if (c == 0xDB) {
      _serialEscaping = true;
    } else {
      if (_serialEscaping) {
        // Rollback explicitly nested 0xDC/0xDD structures mapped from incoming
        // architecture definitions
        if (c == 0xDC)
          c = 0xC0;
        else if (c == 0xDD)
          c = 0xDB;
        _serialEscaping = false;
      }
      if (_serialRxLen < sizeof(_serialRxBuf)) {
        _serialRxBuf[_serialRxLen++] = c;
      } else {
        _serialRxLen = 0; // Terminate overflowing oversized segments gracefully
                          // returning cache back dynamically
      }
    }
  }
}