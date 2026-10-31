#pragma once
#include "Arduino.h"

constexpr int16_t RADIOLIB_ERR_NONE = 0;
constexpr int16_t RADIOLIB_CHANNEL_FREE = 1;
constexpr int16_t RADIOLIB_LORA_DETECTED = 2;

class Module {
 public:
  Module(int, int, int, int) {}
};

class SX1262 {
 public:
  SX1262(Module*) {}
  unsigned rxCalls = 0;
  unsigned txCalls = 0;
  unsigned readCalls = 0;
  bool asleep = false;
  bool listening = false;
  void (*callback)() = nullptr;

  int16_t begin(float, float, uint8_t, uint8_t, uint8_t, int8_t, uint16_t, float) {
    return RADIOLIB_ERR_NONE;
  }
  int16_t setDio2AsRfSwitch(bool) { return RADIOLIB_ERR_NONE; }
  int16_t setRegulatorDCDC() { return RADIOLIB_ERR_NONE; }
  int16_t setCRC(int) { return RADIOLIB_ERR_NONE; }
  void setPacketReceivedAction(void (*action)()) { callback = action; }
  void clearPacketReceivedAction() { callback = nullptr; }
  int16_t startReceive() {
    ++rxCalls;
    asleep = false;
    listening = true;
    return RADIOLIB_ERR_NONE;
  }
  int16_t standby() {
    listening = false;
    return RADIOLIB_ERR_NONE;
  }
  int16_t sleep() {
    asleep = true;
    return RADIOLIB_ERR_NONE;
  }
  size_t getPacketLength() { return 5; }
  int16_t readData(uint8_t*, size_t) {
    ++readCalls;
    return -1;
  }
  int16_t scanChannel() { return RADIOLIB_CHANNEL_FREE; }
  uint32_t getTimeOnAir(size_t) { return 10000; }
  int16_t transmit(const uint8_t*, size_t) {
    ++txCalls;
    return RADIOLIB_ERR_NONE;
  }
};
