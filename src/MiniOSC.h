#ifndef MINIOSC_H
#define MINIOSC_H

#include <stdint.h>
#include <string.h>

struct OSCValue {
  char type; // 'i' for int, 'f' for float
  union {
    int32_t i;
    float f;
  };
};

class MiniOSC {
  public:
    static uint32_t swap32(uint32_t val);
    static int pad(int len);
    static int extract(const uint8_t* data, int len, const char* targetAddress, OSCValue* outArray, int maxArgs);
    static int pack(uint8_t* buffer, const char* address, OSCValue* inArray, int argCount);
};

#endif