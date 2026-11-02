#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace MockPreferences {
inline std::vector<uint8_t> bytes;
inline bool batteryLock = false;
inline bool batteryStorageAvailable = true;
}

class Preferences {
 public:
  bool begin(const char* name, bool) {
    return strcmp(name, "battery") != 0 ||
           MockPreferences::batteryStorageAvailable;
  }
  size_t getBytesLength(const char*) const {
    return MockPreferences::bytes.size();
  }
  size_t getBytes(const char*, void* destination, size_t length) const {
    if (length > MockPreferences::bytes.size()) return 0;
    memcpy(destination, MockPreferences::bytes.data(), length);
    return length;
  }
  size_t putBytes(const char*, const void* source, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(source);
    MockPreferences::bytes.assign(bytes, bytes + length);
    return length;
  }
  bool getBool(const char*, bool defaultValue) const {
    return MockPreferences::batteryLock ? true : defaultValue;
  }
  size_t putBool(const char*, bool value) {
    MockPreferences::batteryLock = value;
    return sizeof(bool);
  }
};
