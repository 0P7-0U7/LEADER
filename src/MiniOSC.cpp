#include "MiniOSC.h"

// Helper to swap endianness for network transmission
uint32_t MiniOSC::swap32(uint32_t val) {
  return ((val << 24) & 0xFF000000) |
         ((val <<  8) & 0x00FF0000) |
         ((val >>  8) & 0x0000FF00) |
         ((val >> 24) & 0x000000FF);
}

int MiniOSC::extract(const uint8_t* data, int len, const char* targetAddress, OSCValue* outArray, int maxArgs) {
  if (len < 4) return 0;
  
  int addrLen = strlen((const char*)data) + 1;
  if (strcmp((const char*)data, targetAddress) != 0) return 0;
  
  // Pad address length to next multiple of 4
  int offset = (addrLen + 3) & ~3; 
  if (offset >= len || data[offset] != ',') return 0;
  
  int typeLen = strlen((const char*)(data + offset)) + 1;
  int typeOffset = offset + 1; 
  int numArgs = typeLen - 2;   
  if (numArgs > maxArgs) numArgs = maxArgs;
  
  // Pad type length to next multiple of 4
  offset += (typeLen + 3) & ~3; 
  
  for (int i = 0; i < numArgs; i++) {
    if (offset + 4 > len) break;
    outArray[i].type = data[typeOffset + i];
    
    uint32_t rawVal;
    memcpy(&rawVal, data + offset, 4);
    outArray[i].i = swap32(rawVal); 
    
    offset += 4;
  }
  
  return numArgs;
}

int MiniOSC::pack(uint8_t* buffer, const char* address, OSCValue* inArray, int argCount) {
  int offset = 0;

  // 1. Address String
  int addrLen = strlen(address);
  memcpy(buffer + offset, address, addrLen);
  offset += addrLen;
  
  // Add null terminator, then pad to multiple of 4
  buffer[offset++] = '\0';
  while (offset % 4 != 0) {
      buffer[offset++] = '\0';
  }

  // 2. Type Tags String
  buffer[offset++] = ',';
  for (int i = 0; i < argCount; i++) {
      buffer[offset++] = inArray[i].type;
  }
  
  // Add null terminator, then pad to multiple of 4
  buffer[offset++] = '\0';
  while (offset % 4 != 0) {
      buffer[offset++] = '\0';
  }

  // 3. Arguments
  for (int i = 0; i < argCount; i++) {
      uint32_t netVal = swap32(inArray[i].i);
      memcpy(buffer + offset, &netVal, 4);
      offset += 4;
  }

  return offset; 
}