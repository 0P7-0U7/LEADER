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
  _autoHop = autoHop;

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

  // If autoHop is enabled, perform an initial channel scan at startup
  if (_autoHop) {
    _lastAutoHopTime = millis();
    triggerHop();
  }
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

void OSCLeader::_sendSlipToSerial(const uint8_t *data, int len) {
  // Worst-case SLIP expansion: each byte becomes 2 bytes + 2 framing bytes
  uint8_t slipBuffer[502]; // 250 * 2 + 2
  int slipIndex = 0;

  slipBuffer[slipIndex++] = 0xC0; // SLIP END (frame start)
  for (int i = 0; i < len; i++) {
    if (slipIndex >= (int)sizeof(slipBuffer) - 2)
      break; // Prevent overflow
    if (data[i] == 0xC0) {
      slipBuffer[slipIndex++] = 0xDB; // ESC
      slipBuffer[slipIndex++] = 0xDC; // ESC_END
    } else if (data[i] == 0xDB) {
      slipBuffer[slipIndex++] = 0xDB; // ESC
      slipBuffer[slipIndex++] = 0xDD; // ESC_ESC
    } else {
      slipBuffer[slipIndex++] = data[i];
    }
  }
  slipBuffer[slipIndex++] = 0xC0; // SLIP END (frame end)

  _serial->write(slipBuffer, slipIndex);
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

  _sendSlipToSerial(outBuffer, outLen);
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

  _sendSlipToSerial(outBuffer, outLen);
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

  // Compact the registry before adding if full
  if (_nodeCount >= MAX_NODES) {
    compactNodeRegistry();
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

void OSCLeader::compactNodeRegistry() {
  uint8_t writeIndex = 0;
  unsigned long currentMillis = millis();

  for (uint8_t i = 0; i < _nodeCount; i++) {
    // Keep nodes that are active and seen within the last 10 seconds
    if (_activeNodes[i].active &&
        (currentMillis - _activeNodes[i].lastSeen <= 10000)) {
      if (writeIndex != i) {
        _activeNodes[writeIndex] = _activeNodes[i];
      }
      writeIndex++;
    }
  }
  _nodeCount = writeIndex;
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

    uint8_t outBuffer[64];
    int outLen = MiniOSC::pack(outBuffer, "/sys/node", outVals, 2);

    _sendSlipToSerial(outBuffer, outLen);
  }
}

bool OSCLeader::update() {
  // Handle asynchronous status LED reset cycle
  if (_ledPin >= 0 && _isLedOn && (millis() - _lastDataTime > _blinkDuration)) {
    digitalWrite(_ledPin, _ledOffState);
    _isLedOn = false;
  }

  // Process queued packets from ESP-NOW callback (thread-safe)
  while (_rxTail != _rxHead) {
    RxPacket &pkt = _rxQueue[_rxTail];

    // Update Node Registry on any incoming message
    uint32_t possibleNodeID = 0;
    if (pkt.len > 9 &&
        strncmp((const char *)pkt.data, "/sys/pong", 9) == 0) {
      OSCValue pongArgs[1];
      int pongCount =
          MiniOSC::extract(pkt.data, pkt.len, "/sys/pong", pongArgs, 1);
      if (pongCount > 0 && pongArgs[0].type == 'i') {
        possibleNodeID = pongArgs[0].i;
      }
    }
    updateNodeRegistry(pkt.mac, possibleNodeID);

    // Forward received radio data to Host Computer via SLIP
    _sendSlipToSerial(pkt.data, pkt.len);

    _rxTail = (_rxTail + 1) % RX_QUEUE_SIZE;
  }

  // Automatic channel hopping on a periodic interval
  if (_autoHop && (millis() - _lastAutoHopTime >= AUTO_HOP_INTERVAL)) {
    _lastAutoHopTime = millis();
    triggerHop();
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
      // Decode escaped specialized bytes inline
      if (_isEscaping) {
        if (incomingByte == 0xDC)
          incomingByte = 0xC0;
        if (incomingByte == 0xDD)
          incomingByte = 0xDB;
        _isEscaping = false;
      }
      // Populate buffer against payload limits
      if (_bufferIndex < sizeof(_oscBuffer)) {
        _oscBuffer[_bufferIndex++] = incomingByte;
      } else {
        _bufferIndex = 0; // Abandon malformed oversized transmissions
      }
    }
  }

  // Update LED indicator on activity
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
  // Queue the packet for processing in update() (main loop context)
  // This avoids serial writes from the Wi-Fi task callback context
  uint8_t nextHead = (_rxHead + 1) % RX_QUEUE_SIZE;
  if (nextHead == _rxTail)
    return; // Queue full, drop packet

  if (len > 250)
    len = 250; // Clamp to ESP-NOW max

  RxPacket &pkt = _rxQueue[_rxHead];
  memcpy(pkt.data, incomingData, len);
  memcpy(pkt.mac, mac, 6);
  pkt.len = len;

  _rxHead = nextHead;
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
}

void OSCFollower::_staticOnDataRecv(const esp_now_recv_info_t *info,
                                    const uint8_t *incomingData, int len) {
  if (_instance)
    _instance->_handleDataRecv(info->src_addr, incomingData, len);
}

void OSCFollower::_handleDataRecv(const uint8_t *mac,
                                  const uint8_t *incomingData, int len) {
  // Queue the packet for processing in update() (main loop context)
  uint8_t nextHead = (_rxHead + 1) % RX_QUEUE_SIZE;
  if (nextHead == _rxTail)
    return; // Queue full, drop packet

  if (len > 250)
    len = 250;

  RxPacket &pkt = _rxQueue[_rxHead];
  memcpy(pkt.data, incomingData, len);
  memcpy(pkt.mac, mac, 6);
  pkt.len = len;

  _rxHead = nextHead;
}

void OSCFollower::update() {
  // Process queued packets from ESP-NOW callback (thread-safe)
  while (_rxTail != _rxHead) {
    RxPacket &pkt = _rxQueue[_rxTail];

    _lastMessageTime = millis();

    // Perform a sanity check before locking onto a presumed Leader node MAC
    if (!_leaderMacSet) {
      bool isValidLeaderPacket = false;

      if (pkt.len == 4 && pkt.data[0] == 0xFE && pkt.data[1] == 0xFE &&
          pkt.data[2] == 0xFE) {
        isValidLeaderPacket = true;
      } else if (pkt.len > 0 && pkt.data[0] == '/') {
        isValidLeaderPacket = true;
      }

      if (isValidLeaderPacket) {
        memcpy(_leaderMac, pkt.mac, 6);
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, _leaderMac, 6);
        peerInfo.channel = _currentChannel;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
        _leaderMacSet = true;
      }
    }

    // Check: Hidden Hardware Hop Command
    if (pkt.len == 4 && pkt.data[0] == 0xFE && pkt.data[1] == 0xFE &&
        pkt.data[2] == 0xFE) {
      uint8_t targetChannel = pkt.data[3];
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
      _rxTail = (_rxTail + 1) % RX_QUEUE_SIZE;
      continue; // Skip downstream processing for hop commands
    }

    // Intercept system ping/pong
    if (pkt.len > 9 &&
        strncmp((const char *)pkt.data, "/sys/ping", 9) == 0) {
      OSCValue pingCheck[1];
      int pingArgs =
          MiniOSC::extract(pkt.data, pkt.len, "/sys/ping", pingCheck, 1);

      if (pingArgs > 0 && pingCheck[0].type == 'i') {
        int interval = pingCheck[0].i;
        if (interval > 0) {
          _heartbeatInterval = interval;
          _heartbeatEnabled = true;
        } else {
          _heartbeatEnabled = false;
        }
      } else if (pingArgs == 0) {
        OSCValue outVal;
        outVal.type = 'i';
        outVal.i = _nodeID;
        uint8_t outBuffer[64];
        int outLen = MiniOSC::pack(outBuffer, "/sys/pong", &outVal, 1);
        send(outBuffer, outLen);
      }
    }

    // Dispatch to user callback
    if (_userCallback)
      _userCallback(pkt.data, pkt.len);

    // Forward to USB if tethered
    if (_usbEnabled) {
      _sendSlipToUSB(pkt.data, pkt.len);
    }

    _rxTail = (_rxTail + 1) % RX_QUEUE_SIZE;
  }

  // Handle serial input for tethered mode
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

  // Worst-case SLIP expansion: each byte becomes 2 bytes + 2 framing bytes
  uint8_t slipBuffer[502]; // 250 * 2 + 2
  int slipIndex = 0;

  slipBuffer[slipIndex++] = 0xC0;
  for (int i = 0; i < len; i++) {
    if (slipIndex >= (int)sizeof(slipBuffer) - 2)
      break; // Prevent overflow
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
        if (c == 0xDC)
          c = 0xC0;
        else if (c == 0xDD)
          c = 0xDB;
        _serialEscaping = false;
      }
      if (_serialRxLen < (int)sizeof(_serialRxBuf)) {
        _serialRxBuf[_serialRxLen++] = c;
      } else {
        _serialRxLen = 0; // Abandon oversized segments
      }
    }
  }
}
