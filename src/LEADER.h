#ifndef LEADER_H
#define LEADER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Stream.h> 

typedef void (*OSCReceiveCallback)(const uint8_t *data, int len);

// ==========================================
// The Universal CNMAT Adaptor Bucket
// ==========================================
class OSCBuffer : public Print {
  public:
    uint8_t buffer[250]; 
    size_t length = 0;
    
    // Catch single bytes
    size_t write(uint8_t b) override {
      if (length < sizeof(buffer)) {
        buffer[length++] = b;
        return 1;
      }
      return 0; 
    }

    // Catch bulk data (Crucial for CNMAT floats/strings)
    size_t write(const uint8_t *str, size_t size) override {
      size_t count = 0;
      while (size--) {
        if (write(*str++)) count++;
        else break;
      }
      return count;
    }
    
    // This fixes the Pure Data "multiple of 4 bytes" error!
    void end() {
      while (length % 4 != 0) {
        write(0); 
      }
    }

    void clear() {
      length = 0;
    }
};

class OSCLeader {
  public:
    void begin(Stream& serialPort, long baudRate = 1000000, uint8_t homeChannel = 1, bool autoHop = false);
    bool update(); 
    void setIndicator(int pin, unsigned long blinkDuration = 20, bool activeLow = true);

  private:
    Stream* _serial;
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

    uint8_t findQuietestChannel();
    void sendChannelFeedback(); // Keeps sending just the channel when hopping
    void sendPingReply();       // Sends the massive 5-argument telemetry list
    void triggerHop();

    static OSCLeader* _instance;
    static void _staticOnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len);
    void _handleDataRecv(const uint8_t *incomingData, int len);
};

class OSCFollower {
  public:
    // --- NEW: Added the enableUSB toggle to the begin function ---
    void begin(uint8_t homeChannel = 1, bool enableUSB = false, long baudRate = 115200);    
    void update();
    void onReceive(OSCReceiveCallback callback);
    void send(const uint8_t *data, int len);
    
    // Heartbeat setup with an optional custom ID (defaults to 0 if left blank)
    void enableHeartbeat(uint32_t interval, uint32_t customID = 0); 
    
  private:
    static OSCFollower* _instance;
    static void _staticOnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len);
    void _handleDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len);

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

    // --- NEW: SLIP USB Variables for Tethered Mode ---
    bool _usbEnabled = false;
    uint8_t _serialRxBuf[512];
    int _serialRxLen = 0;
    bool _serialEscaping = false;
    void _handleSerial();
    void _sendSlipToUSB(const uint8_t *data, int len);
};

#endif