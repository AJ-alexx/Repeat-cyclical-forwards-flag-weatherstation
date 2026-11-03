#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>

#define ARDUINO_ISR_ATTR
constexpr int HIGH = 1;
constexpr int LOW = 0;
constexpr int OUTPUT = 1;
constexpr int INPUT = 0;
constexpr int INPUT_PULLUP = 2;
constexpr int ADC_0db = 0;
constexpr unsigned portNUM_PROCESSORS = 2;
using esp_err_t = int;
using gpio_num_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 1;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 2;
constexpr esp_err_t ESP_ERR_SLEEP_REJECT = 3;
constexpr esp_err_t ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION = 4;
constexpr int ESP_EXT1_WAKEUP_ANY_HIGH = 1;
constexpr int GPIO_INTR_LOW_LEVEL = 5;
constexpr int GPIO_INTR_HIGH_LEVEL = 6;

namespace Mock {
inline uint32_t now = 0;
inline int pins[40] = {};
inline bool radioWakeup = false;
inline bool gpioWakeup = false;
inline int gpioWakeupPin = -1;
inline int gpioWakeupLevel = -1;
inline bool gpioWakeupArmed = false;
inline unsigned activeLevelWakeupSetups = 0;
inline void (*onSleep)() = nullptr;
inline uint64_t sleepDurationUs = 0;
inline uint32_t batteryAdcMillivolts = 684;
inline unsigned sleepCalls = 0;
inline unsigned deepSleepCalls = 0;
inline unsigned watchdogCalls = 0;
inline unsigned restarts = 0;
inline esp_err_t sleepResult = ESP_OK;
inline std::string log;
}

inline uint32_t millis() { return Mock::now; }
inline void delay(uint32_t duration) { Mock::now += duration; }
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { Mock::pins[pin] = value; }
inline int digitalRead(int pin) { return Mock::pins[pin]; }
inline void analogReadResolution(int) {}
inline void analogSetPinAttenuation(int, int) {}
inline uint32_t analogReadMilliVolts(int) {
  return Mock::batteryAdcMillivolts;
}

struct SerialMock {
  void begin(int) {}
  void flush() {}
  void println(const char* text) { Mock::log += std::string(text) + "\n"; }
  void printf(const char* format, ...) {
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    Mock::log += buffer;
  }
};
inline SerialMock Serial;

struct SPIMock {
  void begin(int, int, int, int) {}
};
inline SPIMock SPI;

inline const char* esp_err_to_name(esp_err_t) { return "mock error"; }
inline void esp_restart() { ++Mock::restarts; }
inline uint32_t esp_random() { return 0; }
struct esp_task_wdt_config_t {
  uint32_t timeout_ms;
  unsigned idle_core_mask;
  bool trigger_panic;
};
inline esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) {
  return ESP_OK;
}
inline esp_err_t esp_task_wdt_status(void*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_add(void*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_reset() {
  ++Mock::watchdogCalls;
  return ESP_OK;
}
inline esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t, int) {
  Mock::radioWakeup = true;
  return ESP_OK;
}
inline esp_err_t gpio_wakeup_enable(gpio_num_t pin, int level) {
  Mock::gpioWakeupPin = pin;
  Mock::gpioWakeupLevel = level;
  Mock::gpioWakeupArmed = true;
  if (digitalRead(pin) == (level == GPIO_INTR_LOW_LEVEL ? LOW : HIGH)) {
    ++Mock::activeLevelWakeupSetups;
  }
  return ESP_OK;
}
inline esp_err_t gpio_wakeup_disable(gpio_num_t) {
  Mock::gpioWakeupArmed = false;
  return ESP_OK;
}
inline esp_err_t esp_sleep_enable_gpio_wakeup() {
  Mock::gpioWakeup = true;
  return ESP_OK;
}
inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t duration) {
  Mock::sleepDurationUs = duration;
  return ESP_OK;
}
inline void esp_deep_sleep_start() { ++Mock::deepSleepCalls; }
inline esp_err_t esp_light_sleep_start() {
  ++Mock::sleepCalls;
  if (Mock::onSleep != nullptr) Mock::onSleep();
  if (Mock::sleepResult == ESP_OK) {
    Mock::now += static_cast<uint32_t>(Mock::sleepDurationUs / 1000);
  }
  return Mock::sleepResult;
}
inline esp_err_t gpio_hold_en(gpio_num_t) { return ESP_OK; }
inline esp_err_t gpio_hold_dis(gpio_num_t) { return ESP_OK; }
inline esp_err_t rtc_gpio_hold_dis(gpio_num_t) { return ESP_OK; }
inline esp_err_t rtc_gpio_deinit(gpio_num_t) { return ESP_OK; }
