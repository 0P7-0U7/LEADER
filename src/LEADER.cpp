#include "LEADER.h"
#include "MiniOSC.h" 
#include <esp_mac.h>

// ==========================================
// LEADER IMPLEMENTATION
// ==========================================

OSCLeader* OSCLeader::_instance = nullptr;

void OSCLeader::begin(Stream& serialPort, long baudRate, uint8_t homeChannel, bool autoHop) {
  _instance = this;
  _serial = &serialPort;
  _homeChannel = homeChannel;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE); 
  
  if (esp_now_init() != ESP_OK) return;

  esp_now_register_recv_cb(_staticOnDataRecv);

  esp_wifi_set_channel(_homeChannel, WIFI_SECOND_CHAN_NONE);
  memcpy(_peerInfo.peer_addr, _broadcastAddress, 6);
  _peerInfo.channel = _homeChannel;
  _peerInfo.encrypt = false;
  esp_now_add_peer(&_peerInfo);
}

void OSCLeader::setIndicator(int pin, unsigned long blinkDuration, bool activeLow) {
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

  uint8_t hopMessage[4] = {0xFE, 0xFE, 0xFE, targetChannel}; 
  for(int i = 0; i < 10; i++) {
    esp_now_send(_broadcastAddress, hopMessage, sizeof(hopMessage));
    delay(10); 
  }

  esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
  _peerInfo.channel = targetChannel;
  esp_now_mod_peer(&_peerInfo);

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

void OSCLeader::sendPingReply() {
  OSCValue outVals[5];
  
  outVals[0].type = 'i'; outVals[0].i = _peerInfo.channel; 
  outVals[1].type = 'i'; outVals[1].i = millis() / 1000; 
  outVals[2].type = 'i'; outVals[2].i = ESP.getFreeHeap(); 
  outVals[3].type = 'i'; outVals[3].i = _packetsSent; 
  outVals[4].type = 'i'; outVals[4].i = _packetsDropped; 

  uint8_t outBuffer[128]; 
  int outLen = MiniOSC::pack(outBuffer, "/leader/ping", outVals, 5);

  uint8_t slipBuffer[256]; 
  int slipIndex = 0;

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
  for (int i = 0; i < numNetworks; i++) {
    uint8_t ch = WiFi.channel(i);
    if (ch >= 1 && ch <= 13) channelCounts[ch]++;
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

bool OSCLeader::update() {
  if (_ledPin >= 0 && _isLedOn && (millis() - _lastDataTime > _blinkDuration)) {
    digitalWrite(_ledPin, _ledOffState);
    _isLedOn = false;
  }

  bool actionTriggered = false; 

  while (_serial->available()) {
    uint8_t incomingByte = _serial->read();

    if (incomingByte == 0xC0) { 
      if (_bufferIndex > 0) {
        
        if (strncmp((const char*)_oscBuffer, "/leader/ping", 12) == 0 && _oscBuffer[12] == '\0') {
          sendPingReply(); 
          actionTriggered = true; 
        } 
        else if (strncmp((const char*)_oscBuffer, "/leader/hop", 11) == 0 && _oscBuffer[11] == '\0') {
          triggerHop(); 
          actionTriggered = true; 
        } 
        else {
          esp_err_t result = esp_now_send(_broadcastAddress, _oscBuffer, _bufferIndex);
          if (result == ESP_OK) {
            _packetsSent++;
          } else {
            _packetsDropped++;
          }
          actionTriggered = true; 
        }
        
        _bufferIndex = 0; 
      }
    } else if (incomingByte == 0xDB) { 
      _isEscaping = true;
    } else {
      if (_isEscaping) {
        if (incomingByte == 0xDC) incomingByte = 0xC0;
        if (incomingByte == 0xDD) incomingByte = 0xDB;
        _isEscaping = false;
      }
      if (_bufferIndex < sizeof(_oscBuffer)) {
        _oscBuffer[_bufferIndex++] = incomingByte;
      } else {
        _bufferIndex = 0; 
      }
    }
  }
  
  if (actionTriggered && _ledPin >= 0) {
    digitalWrite(_ledPin, _ledOnState);
    _lastDataTime = millis();
    _isLedOn = true;
  }

  return actionTriggered; 
}

void OSCLeader::_staticOnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (_instance) _instance->_handleDataRecv(incomingData, len);
}

void OSCLeader::_handleDataRecv(const uint8_t *incomingData, int len) {
  uint8_t slipBuffer[512]; 
  int slipIndex = 0;

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
  // 2. NO FLUSH: We removed _serial->flush() here because it 
  // causes packet fragmentation on newer ESP32 Native USB cores.
}

// ==========================================
// FOLLOWER IMPLEMENTATION
// ==========================================

OSCFollower* OSCFollower::_instance = nullptr;

// --- NEW: Added enableUSB parameter ---
void OSCFollower::begin(uint8_t homeChannel, bool enableUSB, long baudRate) {
  _instance = this;
  _homeChannel = homeChannel;
  _currentChannel = homeChannel;
  _usbEnabled = enableUSB; // Save the user's choice

  // --- NEW: Start Serial only if requested ---
  if (_usbEnabled) {
    Serial.begin(baudRate);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE); 
  esp_wifi_set_channel(_homeChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) return;
  
  esp_now_register_recv_cb(_staticOnDataRecv);
  _lastMessageTime = millis();

  // IN THE LIBRARY: Automatically generate the friendly 5-digit ID
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  _nodeID = (mac[4] << 8) | mac[5]; 
}

void OSCFollower::onReceive(OSCReceiveCallback callback) {
  _userCallback = callback;
}

void OSCFollower::send(const uint8_t *data, int len) {
  if (_leaderMacSet) esp_now_send(_leaderMac, data, len);
}

void OSCFollower::enableHeartbeat(uint32_t interval, uint32_t customID) {
  _heartbeatInterval = interval;
  if (customID != 0) {
    _nodeID = customID; 
  }
  _heartbeatEnabled = true;
}

void OSCFollower::_staticOnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (_instance) _instance->_handleDataRecv(info->src_addr, incomingData, len);
}

void OSCFollower::_handleDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  _lastMessageTime = millis();

  if (!_leaderMacSet) {
    memcpy(_leaderMac, mac, 6);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, _leaderMac, 6);
    peerInfo.channel = _currentChannel;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
    _leaderMacSet = true;
  }

  // Check 1: Hidden Hardware Hop Command
  if (len == 4 && incomingData[0] == 0xFE && incomingData[1] == 0xFE && incomingData[2] == 0xFE) {
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
    return; 
  }

  // --- NEW IN LIBRARY: Secretly intercept /sys/ping! ---
  // A quick check to see if the string starts with "/sys/ping"
  if (len > 9 && strncmp((const char*)incomingData, "/sys/ping", 9) == 0) {
    OSCValue pingCheck[1];
    int pingArgs = MiniOSC::extract(incomingData, len, "/sys/ping", pingCheck, 1);
    
    // Scenario A: User sent "/sys/ping 40" (Set speed and start) or "/sys/ping 0" (Stop)
    if (pingArgs > 0 && pingCheck[0].type == 'i') {
      int interval = pingCheck[0].i;
      if (interval > 0) {
        _heartbeatInterval = interval;
        _heartbeatEnabled = true;
      } else {
        _heartbeatEnabled = false; 
      }
    } 
    // Scenario B: User sent just "/sys/ping" with NO numbers (Manual single roll call)
    else if (pingArgs == 0) {
      OSCValue outVal;
      outVal.type = 'i';
      outVal.i = _nodeID;
      uint8_t outBuffer[64];
      int outLen = MiniOSC::pack(outBuffer, "/sys/pong", &outVal, 1);
      send(outBuffer, outLen);
    }
  }

  // Pass the raw data to the user's sketch so they can do their own stuff
  if (_userCallback) _userCallback(incomingData, len);

  // --- NEW: SLIP stream to USB if enabled! ---
  if (_usbEnabled) {
    _sendSlipToUSB(incomingData, len);
  }
}

void OSCFollower::update() {
  // --- NEW: Listen to Pure Data over USB if enabled ---
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
// NEW: FOLLOWER SLIP USB ENGINES
// ==========================================

void OSCFollower::_sendSlipToUSB(const uint8_t *data, int len) {
  if (len == 0 || len % 4 != 0) return; // The Bouncer! Drop corrupted packets

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
        // We received a full packet from Pure Data! Beam it to the Leader.
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
        if (c == 0xDC) c = 0xC0;
        else if (c == 0xDD) c = 0xDB;
        _serialEscaping = false;
      }
      if (_serialRxLen < sizeof(_serialRxBuf)) {
        _serialRxBuf[_serialRxLen++] = c;
      } else {
        _serialRxLen = 0; 
      }
    }
  }
}