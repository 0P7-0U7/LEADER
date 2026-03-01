#ifndef MINIOSC_H
#define MINIOSC_H

#include <stdint.h>
#include <string.h>

/**
 * @brief Structure representing an OSC value (integer or float).
 */
struct OSCValue {
  char type; ///< 'i' for int, 'f' for float, 's' for string, 'T'/'F' for bool,
             ///< 'b' for blob
  union {
    int32_t i;     ///< Integer interpretation of the value
    float f;       ///< Float interpretation of the value
    const char *s; ///< String interpretation of the value
    bool b;        ///< Boolean interpretation (for true/false)
  };
};

/**
 * @brief A lightweight, embedded-friendly Open Sound Control (OSC) parser and
 * packer.
 *
 * Provides static methods to extract OSC arguments from raw byte arrays and
 * pack arguments into a compliant OSC byte format, ensuring 4-byte padding
 * boundaries.
 */
class MiniOSC {
public:
  /**
   * @brief Swaps the endianness of a 32-bit unsigned integer.
   *
   * OSC mandates network byte order (Big-Endian). This function converts
   * between Little-Endian (ESP32 native) and Big-Endian formats.
   *
   * @param val The 32-bit value to swap.
   * @return The endian-swapped 32-bit value.
   */
  static uint32_t swap32(uint32_t val);

  /**
   * @brief Extracts arguments from an incoming OSC byte array.
   *
   * Analyzes an incoming byte array to match a specific OSC address. If it
   * matches, it extracts the arguments safely, guarding against malformed
   * packets.
   *
   * @param data The raw incoming byte array.
   * @param len The length of the incoming byte array.
   * @param targetAddress The OSC address pattern to match (e.g., "/sys/ping").
   * @param outArray Pre-allocated array to store the extracted OSC values.
   * @param maxArgs The maximum number of arguments to extract (prevents buffer
   * overflow).
   * @return The number of arguments successfully extracted.
   */
  static int extract(const uint8_t *data, int len, const char *targetAddress,
                     OSCValue *outArray, int maxArgs);

  /**
   * @brief Packs an address and arguments into a compliant OSC byte array.
   *
   * @param buffer Pre-allocated byte array where the packed OSC message will be
   * written.
   * @param address The OSC address pattern (e.g., "/sensor/pot").
   * @param inArray Array of OSCValue structures containing the arguments to
   * pack.
   * @param argCount The number of arguments provided in the inArray.
   * @return The total length of the packed OSC message in bytes.
   */
  static int pack(uint8_t *buffer, const char *address, OSCValue *inArray,
                  int argCount);
};

#endif