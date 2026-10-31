#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Persist learned schedules; verify them after restart before scheduled RX.
// Every 12 hours, scan continuously for additional stations without clearing
// learned schedules. Names need three receptions; weather needs four.

namespace Fanet {

constexpr size_t MAX_PACKET_SIZE = 255;
constexpr uint8_t FORWARD_FLAG = 0x40;
constexpr uint8_t TYPE_MASK = 0x3F;
constexpr uint8_t NAME = 0x02;
constexpr uint8_t WEATHER = 0x04;
constexpr uint32_t DUPLICATE_WINDOW_MS = 10000;
constexpr uint32_t SLEEP_AWAKE_GUARD_MS = 250;
constexpr uint32_t MIN_TX_PERIOD_MS = 2000;
constexpr uint32_t MAX_TX_PERIOD_MS = 86400000;
constexpr size_t PERIOD_SAMPLES = 3;
constexpr size_t NAME_INTERVALS_REQUIRED = 2;
constexpr uint32_t NAME_INTERVAL_TOLERANCE_PERCENT = 20;
constexpr uint32_t WEATHER_INTERVAL_TOLERANCE_PERCENT = 25;
constexpr uint32_t PERIOD_CORRECTION_DIVISOR = 8;
constexpr uint32_t RX_STARTUP_GUARD_MS = 50;
constexpr uint32_t RX_TIMING_GUARD_MS = 250;
constexpr uint32_t MIN_WINDOW_MARGIN_MS = 1000;
constexpr size_t MAX_SCHEDULES = 32;
constexpr uint32_t RELEARN_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL;
constexpr uint32_t DISCOVERY_QUIET_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t DISCOVERY_MAX_MS = 20UL * 60UL * 1000UL;
constexpr uint32_t CONTROL_MIN_MS = 6UL * 60UL * 1000UL;

class StationDiscovery {
 public:
  void begin(uint32_t now) {
    active_ = true;
    startedAt_ = now;
    lastProtocolReceived_ = now;
  }

  bool active(uint32_t now, bool allLearned = true) {
    if (active_ &&
        (uint32_t(now - startedAt_) >= DISCOVERY_MAX_MS ||
         (allLearned &&
          uint32_t(now - lastProtocolReceived_) >= DISCOVERY_QUIET_MS))) {
      active_ = false;
    }
    return active_;
  }

  void protocolReceived(uint32_t now) {
    if (active_) lastProtocolReceived_ = now;
  }

 private:
  bool active_ = false;
  uint32_t startedAt_ = 0;
  uint32_t lastProtocolReceived_ = 0;
};

inline bool relearningDue(uint32_t now, uint32_t lastLearningStart) {
  return uint32_t(now - lastLearningStart) >= RELEARN_INTERVAL_MS;
}

inline bool mayIdleSleep(uint32_t now, uint32_t lastActivity,
                         bool queueOccupied, bool receivePending) {
  return !queueOccupied && !receivePending &&
         uint32_t(now - lastActivity) >= SLEEP_AWAKE_GUARD_MS;
}

enum class FilterResult { Allowed, Invalid, NoForwardFlag, OtherType };
enum class ReceiveMode { Learning, Listening, Waiting };

struct PersistedSchedule {
  uint8_t address[3] = {};
  uint8_t type = 0;
  uint32_t period = 0;
  uint32_t timingErrorMs = 0;
};

inline FilterResult inspect(const uint8_t* packet, size_t length) {
  if (packet == nullptr || length < 4 || length > MAX_PACKET_SIZE) {
    return FilterResult::Invalid;
  }
  const uint8_t type = packet[0] & TYPE_MASK;
  if (type != NAME && type != WEATHER) {
    return FilterResult::OtherType;
  }
  size_t payload = 4;
  if (packet[0] & 0x80) {
    if (length <= payload) {
      return FilterResult::Invalid;
    }
    const uint8_t extended = packet[payload++];
    if (extended & 0x20) {
      payload += 3;
    }
    if (extended & 0x10) {
      payload += 4;
    }
  }
  if (length <= payload) {
    return FilterResult::Invalid;
  }
  if (type == NAME) {
    return (packet[0] & FORWARD_FLAG) ? FilterResult::Allowed
                                      : FilterResult::NoForwardFlag;
  }

  const uint8_t service = packet[payload];
  // Position plus service byte; the optional service extension precedes position.
  size_t required = 7 + ((service & 0x01) ? 1 : 0);
  if (service & 0x40) required += 1;
  if (service & 0x20) required += 3;
  if (service & 0x10) required += 1;
  if (service & 0x08) required += 2;
  if (service & 0x02) required += 1;
  if (length - payload < required) {
    return FilterResult::Invalid;
  }
  return (packet[0] & FORWARD_FLAG) ? FilterResult::Allowed
                                    : FilterResult::NoForwardFlag;
}

class ReceiveSchedule {
 public:
  bool restore(const PersistedSchedule* schedules, size_t count) {
    reset();
    if (schedules == nullptr || count > MAX_SCHEDULES) return false;
    for (size_t i = 0; i < count; ++i) {
      const PersistedSchedule& schedule = schedules[i];
      if ((schedule.type != NAME && schedule.type != WEATHER) ||
          schedule.period < MIN_TX_PERIOD_MS ||
          schedule.period > MAX_TX_PERIOD_MS ||
          schedule.timingErrorMs > schedule.period) {
        reset();
        return false;
      }
      for (size_t j = 0; j < i; ++j) {
        if (schedules[j].type == schedule.type &&
            memcmp(schedules[j].address, schedule.address,
                   sizeof(schedule.address)) == 0) {
          reset();
          return false;
        }
      }
      Entry* entry = allocate(schedule.address, schedule.type);
      if (entry == nullptr) {
        reset();
        return false;
      }
      entry->period = schedule.period;
      entry->timingErrorMs = schedule.timingErrorMs;
      entry->windowMarginMs = baseWindowMargin(*entry);
      entry->stable = true;
      entry->restored = true;
      entry->verified = false;
      entry->persistedPeriod = schedule.period;
    }
    return true;
  }

  bool observe(const uint8_t* packet, size_t length, uint32_t now) {
    const FilterResult result = inspect(packet, length);
    if (result != FilterResult::Allowed) {
      return false;
    }

    Entry* entry = find(packet);
    if (entry == nullptr) {
      const bool hasWeather = findByAddress(packet + 1, WEATHER) != nullptr;
      const bool hasName = findByAddress(packet + 1, NAME) != nullptr;
      const size_t needed = static_cast<size_t>(!hasWeather) +
                            static_cast<size_t>(!hasName);
      if (trackedCount() > MAX_SCHEDULES - needed) {
        overflow_ = true;
        return false;
      }
      if (!hasWeather) allocate(packet + 1, WEATHER);
      if (!hasName) allocate(packet + 1, NAME);
      entry = find(packet);
    }

    entry->confirmed = true;
    const bool newProtocol = !entry->hasLastSeen && !entry->restored;
    if (entry->hasLastSeen) {
      const uint32_t interval = now - entry->lastSeen;
      // Copies/retries must not move the phase or break a learning sequence.
      if (interval < MIN_TX_PERIOD_MS) return false;
      if (entry->restored && !entry->verified) {
        if (matchesStoredPeriod(*entry, interval)) {
          entry->verified = true;
          entry->restored = false;
          entry->nextAt = now + entry->period;
        } else {
          entry->restored = false;
          entry->stable = false;
          entry->verified = false;
          entry->sampleCount = 0;
          entry->sampleNext = 0;
          relearningDetected_ = true;
          if (interval <= MAX_TX_PERIOD_MS) {
            learnPeriod(*entry, interval);
          }
        }
      } else if (entry->stable) {
        correctPeriod(*entry, interval);
      } else if (interval <= MAX_TX_PERIOD_MS) {
        learnPeriod(*entry, interval);
      } else {
        entry->sampleCount = 0;
        entry->sampleNext = 0;
      }
    }
    entry->lastSeen = now;
    entry->hasLastSeen = true;
    if (entry->stable && entry->verified) {
      entry->nextAt = now + entry->period;
      const uint32_t measuredMargin = baseWindowMargin(*entry);
      const uint32_t relaxedMargin =
          entry->windowMarginMs - entry->windowMarginMs / 8;
      entry->windowMarginMs =
          measuredMargin > relaxedMargin ? measuredMargin : relaxedMargin;
    }
    return newProtocol;
  }

  ReceiveMode mode(uint32_t now, uint32_t& waitMs,
                   bool allowIncomplete = false) {
    waitMs = 0;
    if (!allowIncomplete && (overflow_ || !ready())) {
      return ReceiveMode::Learning;
    }

    bool listening = false;
    uint32_t shortestWait = UINT32_MAX;
    for (Entry& entry : entries_) {
      if (!entry.used || !entry.stable || !entry.verified) continue;
      uint32_t opensAt =
          entry.nextAt - entry.windowMarginMs - RX_STARTUP_GUARD_MS;
      uint32_t closesAt = entry.nextAt + entry.windowMarginMs;
      uint8_t skippedWindows = 0;
      while (int32_t(now - closesAt) > 0 && skippedWindows < 32) {
        entry.windowMarginMs = widenedWindowMargin(entry.windowMarginMs,
                                                   entry.period);
        entry.nextAt += entry.period;
        opensAt = entry.nextAt - entry.windowMarginMs - RX_STARTUP_GUARD_MS;
        closesAt = entry.nextAt + entry.windowMarginMs;
        ++skippedWindows;
      }
      if (int32_t(now - opensAt) >= 0 && int32_t(closesAt - now) >= 0) {
        listening = true;
      } else if (int32_t(now - closesAt) > 0) {
        // After many missed cycles, keep the learned phase instead of
        // inventing a new phase from the time at which mode() was called.
        const uint32_t skipped = static_cast<uint32_t>(
            (static_cast<uint64_t>(uint32_t(now - closesAt)) +
             entry.period - 1) / entry.period);
        entry.nextAt += skipped * entry.period;
        opensAt = entry.nextAt - entry.windowMarginMs - RX_STARTUP_GUARD_MS;
        closesAt = entry.nextAt + entry.windowMarginMs;
      }
      if (int32_t(now - opensAt) < 0) {
        const int32_t remaining = int32_t(opensAt - now);
        if (remaining > 0 && static_cast<uint32_t>(remaining) < shortestWait) {
          shortestWait = static_cast<uint32_t>(remaining);
        }
      } else if (int32_t(closesAt - now) >= 0) {
        listening = true;
      }
    }
    if (listening) return ReceiveMode::Listening;
    if (shortestWait == UINT32_MAX) {
      if (allowIncomplete) {
        // With no learned periods, retry discovery at the next daily scan.
        waitMs = 60000;
        return ReceiveMode::Waiting;
      }
      return ReceiveMode::Learning;
    }
    waitMs = shortestWait;
    return ReceiveMode::Waiting;
  }

  size_t trackedCount() const {
    size_t count = 0;
    for (const Entry& entry : entries_) {
      if (entry.used) ++count;
    }
    return count;
  }

  size_t learnedCount() const {
    size_t count = 0;
    for (const Entry& entry : entries_) {
      if (entry.used && entry.stable) ++count;
    }
    return count;
  }

  bool capacityExceeded() const {
    return overflow_;
  }

  bool complete() const {
    return !overflow_ && ready();
  }

  // Starts a control phase; only stations heard during it are kept.
  void beginControl(uint32_t now) {
    controlStart_ = now;
    for (Entry& entry : entries_) entry.confirmed = false;
  }

  // Continuous RX for at least CONTROL_MIN_MS. Unheard stations are awaited
  // for one full interval of each learned protocol; heard stored protocols
  // (including NAME) for two packets so their interval can be checked.
  bool controlPending(uint32_t now) const {
    const uint64_t elapsed = uint32_t(now - controlStart_);
    if (elapsed < CONTROL_MIN_MS) return true;
    for (const Entry& entry : entries_) {
      if (!entry.used || !entry.stable) continue;
      const bool heard = stationHeard(entry.address);
      const bool unverified = entry.restored && !entry.verified;
      if (heard && !unverified) continue;
      const uint64_t tolerance =
          static_cast<uint64_t>(entry.period) * intervalTolerance(entry) / 100 +
          entry.windowMarginMs;
      const uint64_t needed = heard ? 2ULL * entry.period + tolerance
                                    : static_cast<uint64_t>(entry.period) + tolerance;
      if (elapsed < needed) return true;
    }
    return false;
  }

  // Removes every station from which not a single packet arrived.
  size_t pruneUnheardStations() {
    size_t removed = 0;
    for (Entry& entry : entries_) {
      if (!entry.used || stationHeard(entry.address)) continue;
      const uint8_t address[3] = {entry.address[0], entry.address[1],
                                  entry.address[2]};
      for (Entry& other : entries_) {
        if (other.used &&
            memcmp(other.address, address, sizeof(address)) == 0) {
          other = Entry{};
        }
      }
      persistenceDirty_ = true;
      ++removed;
    }
    return removed;
  }

  bool takeRelearningDetected() {
    const bool result = relearningDetected_;
    relearningDetected_ = false;
    return result;
  }

  bool hasUnverifiedRestored() const {
    for (const Entry& entry : entries_) {
      if (entry.used && entry.restored && !entry.verified) return true;
    }
    return false;
  }

  void discardUnverifiedRestored() {
    for (Entry& entry : entries_) {
      if (entry.used && entry.restored && !entry.verified) {
        entry = Entry{};
        persistenceDirty_ = true;
      }
    }
  }

  size_t exportSchedules(PersistedSchedule* schedules, size_t capacity) const {
    size_t count = 0;
    for (const Entry& entry : entries_) {
      if (!entry.used || !entry.stable) continue;
      if (count == capacity) return capacity + 1;
      memcpy(schedules[count].address, entry.address, sizeof(entry.address));
      schedules[count].type = entry.type;
      schedules[count].period = entry.period;
      schedules[count].timingErrorMs = entry.timingErrorMs;
      ++count;
    }
    return count;
  }

  bool persistenceDirty() const { return persistenceDirty_; }

  void markPersisted() {
    for (Entry& entry : entries_) {
      if (entry.used && entry.stable && entry.verified) {
        entry.persistedPeriod = entry.period;
      }
    }
    persistenceDirty_ = false;
  }

  void reset() {
    for (Entry& entry : entries_) {
      entry = Entry{};
    }
    overflow_ = false;
    persistenceDirty_ = false;
    relearningDetected_ = false;
    controlStart_ = 0;
  }

 private:
  struct Entry {
    uint8_t address[3] = {};
    uint8_t type = 0;
    uint32_t lastSeen = 0;
    uint32_t nextAt = 0;
    uint32_t period = 0;
    uint32_t windowMarginMs = 0;
    uint32_t timingErrorMs = 0;
    uint32_t persistedPeriod = 0;
    uint32_t samples[PERIOD_SAMPLES] = {};
    size_t sampleCount = 0;
    size_t sampleNext = 0;
    bool used = false;
    bool hasLastSeen = false;
    bool stable = false;
    bool restored = false;
    bool verified = true;
    bool confirmed = false;
  };

  bool stationHeard(const uint8_t* address) const {
    for (const Entry& entry : entries_) {
      if (entry.used && entry.confirmed &&
          memcmp(entry.address, address, sizeof(entry.address)) == 0) {
        return true;
      }
    }
    return false;
  }

  Entry* find(const uint8_t* packet) {
    const uint8_t type = packet[0] & TYPE_MASK;
    return findByAddress(packet + 1, type);
  }

  Entry* findByAddress(const uint8_t* address, uint8_t type) {
    for (Entry& entry : entries_) {
      if (entry.used && entry.type == type &&
          memcmp(entry.address, address, sizeof(entry.address)) == 0) {
        return &entry;
      }
    }
    return nullptr;
  }

  Entry* allocate(const uint8_t* address, uint8_t type) {
    for (Entry& entry : entries_) {
      if (entry.used) continue;
      entry = Entry{};
      memcpy(entry.address, address, sizeof(entry.address));
      entry.type = type;
      entry.used = true;
      return &entry;
    }
    return nullptr;
  }

  bool ready() const {
    bool found = false;
    for (const Entry& entry : entries_) {
      if (!entry.used) continue;
      found = true;
      if (!entry.stable || !entry.verified) return false;
    }
    return found;
  }

  static uint32_t baseWindowMargin(const Entry& entry) {
    uint32_t margin = entry.timingErrorMs * 2 + RX_TIMING_GUARD_MS;
    const uint32_t maximum = maximumWindowMargin(entry.period);
    if (margin < MIN_WINDOW_MARGIN_MS) margin = MIN_WINDOW_MARGIN_MS;
    return margin > maximum ? maximum : margin;
  }

  static uint32_t widenedWindowMargin(uint32_t margin, uint32_t period) {
    const uint32_t maximum = maximumWindowMargin(period);
    if (margin >= maximum) return maximum;
    return margin > maximum / 2 ? maximum : margin * 2;
  }

  static uint32_t maximumWindowMargin(uint32_t period) {
    // At this limit adjacent windows touch: a lost phase cannot leave a
    // permanent blind gap, even if the transmitter has changed its rhythm.
    return period / 2;
  }

  static uint32_t intervalTolerance(const Entry& entry) {
    return entry.type == NAME ? NAME_INTERVAL_TOLERANCE_PERCENT
                              : WEATHER_INTERVAL_TOLERANCE_PERCENT;
  }

  static bool matchesStoredPeriod(const Entry& entry, uint32_t interval) {
    const uint32_t difference = interval > entry.period
                                    ? interval - entry.period
                                    : entry.period - interval;
    return static_cast<uint64_t>(difference) * 100 <=
           static_cast<uint64_t>(entry.period) * intervalTolerance(entry);
  }

  void learnPeriod(Entry& entry, uint32_t interval) {
    const size_t required =
        entry.type == NAME ? NAME_INTERVALS_REQUIRED : PERIOD_SAMPLES;
    if (entry.type == NAME && entry.sampleCount != 0) {
      const uint32_t previous =
          entry.samples[(entry.sampleNext + required - 1) % required];
      const uint32_t difference =
          interval > previous ? interval - previous : previous - interval;
      if (static_cast<uint64_t>(difference) * 100 >
          static_cast<uint64_t>(previous) * NAME_INTERVAL_TOLERANCE_PERCENT) {
        entry.sampleCount = 0;
        entry.sampleNext = 0;
      }
    }
    entry.samples[entry.sampleNext] = interval;
    entry.sampleNext = (entry.sampleNext + 1) % required;
    if (entry.sampleCount < required) ++entry.sampleCount;
    if (entry.sampleCount < required) return;

    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < required; ++i) {
      const uint32_t sample = entry.samples[i];
      if (sample < minimum) minimum = sample;
      if (sample > maximum) maximum = sample;
      sum += sample;
    }
    const uint32_t period = static_cast<uint32_t>(sum / required);
    if (entry.type == WEATHER &&
        static_cast<uint64_t>(maximum - minimum) * 100 >
        static_cast<uint64_t>(period) * intervalTolerance(entry)) {
      return;
    }
    entry.period = period;
    entry.timingErrorMs =
        maximum - period > period - minimum ? maximum - period : period - minimum;
    entry.windowMarginMs = 0;
    entry.stable = true;
    entry.verified = true;
    entry.restored = false;
    entry.persistedPeriod = 0;
    persistenceDirty_ = true;
  }

  void correctPeriod(Entry& entry, uint32_t interval) {
    // Compare against the original prediction, even when mode() has already
    // advanced nextAt past missed windows. A gap of N cycles is not a new period.
    uint32_t cycles = static_cast<uint32_t>(
        (static_cast<uint64_t>(interval) + entry.period / 2) / entry.period);
    if (cycles == 0) cycles = 1;
    const uint32_t measuredPeriod = interval / cycles;
    const int64_t difference =
        static_cast<int64_t>(measuredPeriod) - entry.period;
    const uint32_t deviation =
        static_cast<uint32_t>(difference < 0 ? -difference : difference);
    if (interval > INT32_MAX || measuredPeriod < MIN_TX_PERIOD_MS ||
        measuredPeriod > MAX_TX_PERIOD_MS ||
        static_cast<uint64_t>(deviation) * 100 >
            static_cast<uint64_t>(entry.period) * intervalTolerance(entry)) {
      entry.stable = false;
      entry.sampleCount = 0;
      entry.sampleNext = 0;
      persistenceDirty_ = true;
      return;
    }

    const int64_t phaseError = static_cast<int64_t>(interval) -
                              static_cast<int64_t>(cycles) * entry.period;
    const uint32_t error =
        static_cast<uint32_t>(phaseError < 0 ? -phaseError : phaseError);
    const uint32_t decayedError = entry.timingErrorMs - entry.timingErrorMs / 8;
    entry.timingErrorMs = error > decayedError ? error : decayedError;
    entry.period = static_cast<uint32_t>(
        static_cast<int64_t>(entry.period) +
        difference / PERIOD_CORRECTION_DIVISOR);
    const uint32_t persistedDifference =
        entry.period > entry.persistedPeriod
            ? entry.period - entry.persistedPeriod
            : entry.persistedPeriod - entry.period;
    if (entry.persistedPeriod == 0 ||
        static_cast<uint64_t>(persistedDifference) * 100 >= entry.period) {
      persistenceDirty_ = true;
    }
  }

  Entry entries_[MAX_SCHEDULES] = {};
  bool overflow_ = false;
  bool persistenceDirty_ = false;
  bool relearningDetected_ = false;
  uint32_t controlStart_ = 0;
};

inline bool prepareForRelay(uint8_t* packet, size_t length) {
  if (inspect(packet, length) != FilterResult::Allowed) {
    return false;
  }
  packet[0] &= static_cast<uint8_t>(~FORWARD_FLAG);
  return true;
}

inline bool samePacket(const uint8_t* a, size_t aLength,
                       const uint8_t* b, size_t bLength) {
  return aLength >= 4 && aLength == bLength &&
         (a[0] & ~FORWARD_FLAG) == (b[0] & ~FORWARD_FLAG) &&
         memcmp(a + 1, b + 1, aLength - 1) == 0;
}

class RecentPackets {
 public:
  bool contains(const uint8_t* packet, size_t length, uint32_t now) const {
    for (const Entry& entry : entries_) {
      if (entry.length != 0 && uint32_t(now - entry.sentAt) < DUPLICATE_WINDOW_MS &&
          samePacket(packet, length, entry.data, entry.length)) {
        return true;
      }
    }
    return false;
  }

  void remember(const uint8_t* packet, size_t length, uint32_t now) {
    if (length < 4 || length > MAX_PACKET_SIZE) {
      return;
    }
    Entry& entry = entries_[next_];
    memcpy(entry.data, packet, length);
    entry.length = length;
    entry.sentAt = now;
    next_ = (next_ + 1) % CAPACITY;
  }

 private:
  static constexpr size_t CAPACITY = 16;
  struct Entry {
    uint8_t data[MAX_PACKET_SIZE] = {};
    size_t length = 0;
    uint32_t sentAt = 0;
  };
  Entry entries_[CAPACITY] = {};
  size_t next_ = 0;
};

}  // namespace Fanet

#ifndef FANET_PROTOCOL_TEST

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#ifndef ENABLE_LIGHT_SLEEP
#define ENABLE_LIGHT_SLEEP 1
#endif

#ifndef ENABLE_ADAPTIVE_RX
#define ENABLE_ADAPTIVE_RX 1
#endif

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#if !defined(ARDUINO_ARCH_ESP32) || !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select Heltec WiFi LoRa 32(V3) with ESP32-S3 in the Arduino IDE."
#endif

// EU FANET profile, matching breezedudeWindSensor.
constexpr float FREQUENCY_MHZ = 868.2f;
constexpr float BANDWIDTH_KHZ = 250.0f;
constexpr uint8_t SPREADING_FACTOR = 7;
constexpr uint8_t CODING_RATE = 5;
constexpr uint8_t SYNC_WORD = 0xF1;
constexpr int8_t TX_POWER_DBM = 13;
constexpr uint16_t PREAMBLE_SYMBOLS = 12;

constexpr int PIN_SCK = 9;
constexpr int PIN_MISO = 11;
constexpr int PIN_MOSI = 10;
constexpr int PIN_CS = 8;
constexpr int PIN_RESET = 12;
constexpr int PIN_DIO1 = 14;
constexpr int PIN_BUSY = 13;
constexpr int PIN_LED = 35;
constexpr int PIN_AWAKE_LED = 4;
constexpr int PIN_VEXT = 36;

constexpr size_t QUEUE_SIZE = 8;
constexpr uint32_t PACKET_LIFETIME_MS = 30000;
constexpr uint32_t STATUS_INTERVAL_MS = 60000;
constexpr uint32_t WATCHDOG_TIMEOUT_MS = 90000;

SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RESET, PIN_BUSY);
volatile bool receivedFlag = false;
Fanet::RecentPackets recentPackets;
Fanet::ReceiveSchedule receiveSchedule;
Fanet::StationDiscovery stationDiscovery;
bool radioSleeping = false;
bool controlActive = false;
uint32_t lastLearningStart = 0;

// Every learning phase is also a control phase: continuous RX for at least
// six minutes; stations not heard during it are deleted at its end.
void beginLearning(uint32_t now) {
  lastLearningStart = now;
  stationDiscovery.begin(now);
  if (!controlActive) {
    controlActive = true;
    receiveSchedule.beginControl(now);
  }
}

struct PendingPacket {
  uint8_t data[Fanet::MAX_PACKET_SIZE] = {};
  size_t length = 0;
  uint32_t queuedAt = 0;
  uint32_t attemptAt = 0;
  uint32_t waitMs = 0;
};

PendingPacket pending[QUEUE_SIZE];
uint32_t receivedCount = 0;
uint32_t forwardedCount = 0;
uint32_t filteredCount = 0;
uint32_t duplicateCount = 0;
uint32_t droppedCount = 0;
uint32_t lastTxFinished = 0;
uint32_t txOffTimeMs = 0;
uint32_t lastStatus = 0;
uint32_t lastRadioActivity = 0;

namespace {
constexpr uint32_t SCHEDULE_STORAGE_MAGIC = 0x46414E54;
constexpr uint16_t SCHEDULE_STORAGE_VERSION = 1;
constexpr char SCHEDULE_STORAGE_KEY[] = "schedule";

struct ScheduleStorageImage {
  uint32_t magic = SCHEDULE_STORAGE_MAGIC;
  uint16_t version = SCHEDULE_STORAGE_VERSION;
  uint16_t count = 0;
  Fanet::PersistedSchedule schedules[Fanet::MAX_SCHEDULES] = {};
};

Preferences schedulePreferences;
bool scheduleStorageReady = false;

bool loadPersistentSchedule() {
  if (!schedulePreferences.begin("fanet-cadence", false)) {
    Serial.println("ERROR: could not open persistent cadence storage.");
    return false;
  }
  scheduleStorageReady = true;
  const size_t storedSize =
      schedulePreferences.getBytesLength(SCHEDULE_STORAGE_KEY);
  if (storedSize == 0) return false;
  if (storedSize != sizeof(ScheduleStorageImage)) {
    Serial.println("WARNING: stored cadence data has an unsupported size.");
    return false;
  }

  ScheduleStorageImage image;
  if (schedulePreferences.getBytes(SCHEDULE_STORAGE_KEY, &image,
                                   sizeof(image)) != sizeof(image) ||
      image.magic != SCHEDULE_STORAGE_MAGIC ||
      image.version != SCHEDULE_STORAGE_VERSION ||
      image.count > Fanet::MAX_SCHEDULES ||
      !receiveSchedule.restore(image.schedules, image.count)) {
    Serial.println("WARNING: stored cadence data is invalid; relearning.");
    receiveSchedule.reset();
    return false;
  }
  Serial.printf("Restored %u learned station protocols; verifying cadence in "
                "continuous RX.\n",
                static_cast<unsigned>(image.count));
  return true;
}

void persistScheduleIfNeeded() {
  if (!scheduleStorageReady || !receiveSchedule.persistenceDirty()) return;
  ScheduleStorageImage image;
  image.count = static_cast<uint16_t>(
      receiveSchedule.exportSchedules(image.schedules, Fanet::MAX_SCHEDULES));
  if (image.count > Fanet::MAX_SCHEDULES) {
    Serial.println("ERROR: cadence storage capacity exceeded.");
    return;
  }
  const size_t written =
      schedulePreferences.putBytes(SCHEDULE_STORAGE_KEY, &image, sizeof(image));
  if (written != sizeof(image)) {
    Serial.println("ERROR: could not persist learned station cadence.");
    scheduleStorageReady = false;
    return;
  }
  receiveSchedule.markPersisted();
}
}  // namespace

void ARDUINO_ISR_ATTR onPacketReceived() {
  receivedFlag = true;
}

void fatalError(const char* operation, int16_t code) {
  Serial.printf("FATAL: %s, RadioLib code=%d. Check board/antenna and reset.\n",
                operation, code);
  while (true) {
    digitalWrite(PIN_LED, HIGH);
    delay(200);
    digitalWrite(PIN_LED, LOW);
    delay(800);
  }
}

void requireRadio(const char* operation, int16_t code) {
  if (code != RADIOLIB_ERR_NONE) {
    fatalError(operation, code);
  }
}

void watchdogSetupFailed(esp_err_t error) {
  Serial.printf("FATAL: watchdog setup failed: %s (%d). Restarting.\n",
                esp_err_to_name(error), static_cast<int>(error));
  Serial.flush();
  delay(100);
  esp_restart();
}

void initializeWatchdog() {
  esp_task_wdt_config_t config = {};
  config.timeout_ms = WATCHDOG_TIMEOUT_MS;
  config.idle_core_mask = (1U << portNUM_PROCESSORS) - 1U;
  config.trigger_panic = true;
  esp_err_t result = esp_task_wdt_init(&config);
  if (result == ESP_ERR_INVALID_STATE) {
    result = esp_task_wdt_reconfigure(&config);
  }
  if (result != ESP_OK) {
    watchdogSetupFailed(result);
    return;
  }

  result = esp_task_wdt_status(nullptr);
  if (result == ESP_ERR_NOT_FOUND) {
    result = esp_task_wdt_add(nullptr);
  }
  if (result != ESP_OK) {
    watchdogSetupFailed(result);
    return;
  }
  result = esp_task_wdt_reset();
  if (result != ESP_OK) {
    watchdogSetupFailed(result);
    return;
  }
  Serial.printf("Task watchdog enabled: %lu s timeout; stalled main loop restarts.\n",
                static_cast<unsigned long>(WATCHDOG_TIMEOUT_MS / 1000));
}

void startReceiving() {
  receivedFlag = false;
  radio.setPacketReceivedAction(onPacketReceived);
  requireRadio("startReceive", radio.startReceive());
  radioSleeping = false;
}

void enqueuePacket(uint8_t* packet, size_t length) {
  const Fanet::FilterResult result = Fanet::inspect(packet, length);
  if (result != Fanet::FilterResult::Allowed) {
    ++filteredCount;
    if (result == Fanet::FilterResult::Invalid) {
      Serial.println("DROP: truncated/invalid FANET packet.");
    }
    return;
  }

  const uint32_t now = millis();
  if (recentPackets.contains(packet, length, now)) {
    ++duplicateCount;
    return;
  }
  PendingPacket* freeSlot = nullptr;
  for (PendingPacket& slot : pending) {
    if (slot.length == 0) {
      if (freeSlot == nullptr) freeSlot = &slot;
    } else if (Fanet::samePacket(packet, length, slot.data, slot.length)) {
      ++duplicateCount;
      return;
    }
  }
  if (freeSlot == nullptr) {
    ++droppedCount;
    Serial.println("DROP: relay queue full.");
    return;
  }
  if (!Fanet::prepareForRelay(packet, length)) {
    ++droppedCount;
    Serial.println("ERROR: relay preparation rejected packet.");
    return;
  }
  memcpy(freeSlot->data, packet, length);
  freeSlot->length = length;
  freeSlot->queuedAt = now;
  freeSlot->attemptAt = now;
  freeSlot->waitMs = 500 + esp_random() % 1001;
  Serial.printf("Queued: type=0x%02X sender=%02X:%02X%02X bytes=%u, forward=0\n",
                packet[0] & Fanet::TYPE_MASK, packet[1], packet[3], packet[2],
                static_cast<unsigned>(length));
}

void receivePacket() {
  if (!receivedFlag && digitalRead(PIN_DIO1) == LOW) {
    return;
  }
  const uint32_t receivedAt = millis();
  lastRadioActivity = receivedAt;
  radio.clearPacketReceivedAction();
  receivedFlag = false;
  const size_t length = radio.getPacketLength();
  uint8_t packet[Fanet::MAX_PACKET_SIZE];
  if (length == 0 || length > sizeof(packet)) {
    startReceiving();
    ++droppedCount;
    Serial.printf("DROP: invalid radio packet length=%u\n", static_cast<unsigned>(length));
  } else {
    const int16_t state = radio.readData(packet, length);
    startReceiving();
    if (state == RADIOLIB_ERR_NONE) {
      ++receivedCount;
      if (Fanet::inspect(packet, length) == Fanet::FilterResult::Allowed) {
        const bool previousOverflow = receiveSchedule.capacityExceeded();
        const size_t previouslyLearned = receiveSchedule.learnedCount();
        const bool newProtocol = receiveSchedule.observe(packet, length, receivedAt);
        const bool changedProtocol = receiveSchedule.takeRelearningDetected();
        if (newProtocol || changedProtocol ||
            receiveSchedule.learnedCount() < previouslyLearned) {
          beginLearning(receivedAt);
          Serial.println("Cadence learning: continuous RX for new/changed protocol.");
        }
        stationDiscovery.protocolReceived(receivedAt);
        if (newProtocol) {
          Serial.println("New forwarded protocol: learning its send interval.");
        }
        if (!previousOverflow && receiveSchedule.capacityExceeded()) {
          Serial.println("WARNING: station schedule capacity exceeded; "
                         "additional stations cannot be scheduled.");
        }
        persistScheduleIfNeeded();
      }
      enqueuePacket(packet, length);
    } else {
      ++droppedCount;
      Serial.printf("DROP: RX error, RadioLib code=%d\n", state);
    }
  }
}

void forwardPacket() {
  const uint32_t now = millis();
  PendingPacket* ready = nullptr;
  for (PendingPacket& slot : pending) {
    if (slot.length == 0) continue;
    if (uint32_t(now - slot.queuedAt) >= PACKET_LIFETIME_MS) {
      slot.length = 0;
      ++droppedCount;
      Serial.println("DROP: queued packet expired (channel busy/duty-cycle limit).");
    } else if (ready == nullptr && uint32_t(now - slot.attemptAt) >= slot.waitMs) {
      ready = &slot;
    }
  }
  if (ready == nullptr || uint32_t(now - lastTxFinished) < txOffTimeMs) {
    return;
  }

  // Freeze RX first; preserve a completed reception instead of starting CAD over it.
  requireRadio("standby", radio.standby());
  if (receivedFlag || digitalRead(PIN_DIO1) == HIGH) {
    receivedFlag = true;
    return;
  }
  radio.clearPacketReceivedAction();
  const int16_t scan = radio.scanChannel();
  if (scan != RADIOLIB_CHANNEL_FREE) {
    if (scan != RADIOLIB_LORA_DETECTED) {
      Serial.printf("ERROR: channel scan, RadioLib code=%d\n", scan);
    }
    ready->attemptAt = millis();
    ready->waitMs = 500 + esp_random() % 1501;
    startReceiving();
    return;
  }

  // The queued packet has already had its forward flag removed.
  if ((ready->data[0] & Fanet::FORWARD_FLAG) != 0 ||
      ((ready->data[0] & Fanet::TYPE_MASK) != Fanet::WEATHER &&
       (ready->data[0] & Fanet::TYPE_MASK) != Fanet::NAME)) {
    fatalError("relay invariant", -1);
  }
  const uint32_t airtimeMs = (radio.getTimeOnAir(ready->length) + 999UL) / 1000UL;
  digitalWrite(PIN_LED, HIGH);
  const uint32_t started = millis();
  const int16_t state = radio.transmit(ready->data, ready->length);
  lastTxFinished = millis();
  lastRadioActivity = lastTxFinished;
  const uint32_t elapsed = lastTxFinished - started;
  // At least 99 times the airtime off: at most 1% TX, including failed attempts.
  txOffTimeMs = (elapsed > airtimeMs ? elapsed : airtimeMs) * 99UL;
  digitalWrite(PIN_LED, LOW);
  startReceiving();
  if (state == RADIOLIB_ERR_NONE) {
    ++forwardedCount;
    recentPackets.remember(ready->data, ready->length, lastTxFinished);
    Serial.printf("TX OK: type=0x%02X bytes=%u forward=0, off-time=%lu ms\n",
                  ready->data[0] & Fanet::TYPE_MASK,
                  static_cast<unsigned>(ready->length),
                  static_cast<unsigned long>(txOffTimeMs));
  } else {
    ++droppedCount;
    Serial.printf("DROP: TX error, RadioLib code=%d\n", state);
  }
  ready->length = 0;
}

bool lightSleepReady = false;
constexpr uint64_t LIGHT_SLEEP_MAX_US = 100000;
constexpr uint32_t SCHEDULED_SLEEP_MAX_MS = 60000;

void disableLightSleep(const char* operation, esp_err_t error) {
  lightSleepReady = false;
  Serial.printf("ERROR: light sleep %s: %s (%d). Staying awake until reset.\n",
                operation, esp_err_to_name(error), static_cast<int>(error));
}

void initLightSleep() {
  // Separate RTC wakeup preserves RadioLib's rising-edge GPIO interrupt.
  const esp_err_t error = esp_sleep_enable_ext1_wakeup_io(
      1ULL << PIN_DIO1, ESP_EXT1_WAKEUP_ANY_HIGH);
  if (error != ESP_OK) {
    disableLightSleep("wakeup setup", error);
    return;
  }
  lightSleepReady = true;
  Serial.println("Light sleep ready: radio wakeup enabled.");
}

void enterLightSleep(uint64_t durationUs) {
  if (!lightSleepReady) return;
  Serial.flush();
  const esp_err_t timer = esp_sleep_enable_timer_wakeup(durationUs);
  if (timer != ESP_OK) {
    disableLightSleep("timer setup", timer);
    return;
  }
  if (receivedFlag || digitalRead(PIN_DIO1) == HIGH) return;

  digitalWrite(PIN_AWAKE_LED, LOW);
  const esp_err_t hold = gpio_hold_en(static_cast<gpio_num_t>(PIN_AWAKE_LED));
  if (hold != ESP_OK) {
    digitalWrite(PIN_AWAKE_LED, HIGH);
    disableLightSleep("awake LED hold", hold);
    return;
  }
  const esp_err_t result = esp_light_sleep_start();
  const esp_err_t ledRelease =
      gpio_hold_dis(static_cast<gpio_num_t>(PIN_AWAKE_LED));
  if (ledRelease != ESP_OK) {
    disableLightSleep("awake LED hold release", ledRelease);
  }
  digitalWrite(PIN_AWAKE_LED, HIGH);
  const esp_err_t release = rtc_gpio_hold_dis(static_cast<gpio_num_t>(PIN_DIO1));
  if (release != ESP_OK) {
    disableLightSleep("DIO1 hold release", release);
  }
  const esp_err_t restore = rtc_gpio_deinit(static_cast<gpio_num_t>(PIN_DIO1));
  if (restore != ESP_OK) {
    disableLightSleep("DIO1 restore", restore);
  }
  if (digitalRead(PIN_DIO1) == HIGH) receivedFlag = true;
  if (result != ESP_OK && result != ESP_ERR_SLEEP_REJECT &&
      result != ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION) {
    disableLightSleep("entry", result);
  }
}

#if ENABLE_LIGHT_SLEEP
void idleLightSleep() {
  bool queueOccupied = false;
  for (const PendingPacket& slot : pending) {
    if (slot.length != 0) {
      queueOccupied = true;
      break;
    }
  }

  if (!lightSleepReady) {
    if (radioSleeping) {
      receiveSchedule.reset();
      startReceiving();
    }
    return;
  }

#if ENABLE_ADAPTIVE_RX
  uint32_t waitMs = 0;
  const uint32_t now = millis();
  if (controlActive && !receiveSchedule.controlPending(now)) {
    controlActive = false;
    const size_t removedStations = receiveSchedule.pruneUnheardStations();
    const bool relearn = receiveSchedule.hasUnverifiedRestored();
    receiveSchedule.discardUnverifiedRestored();
    persistScheduleIfNeeded();
    Serial.printf("Control finished: %u unheard station(s) deleted.\n",
                  static_cast<unsigned>(removedStations));
    if (relearn) {
      beginLearning(now);
      Serial.println("Stored cadence not confirmed; starting normal learning.");
      return;
    }
  }
  const bool discovering =
      controlActive ||
      stationDiscovery.active(now, receiveSchedule.complete());
  const Fanet::ReceiveMode mode = receiveSchedule.mode(now, waitMs, !discovering);
  if (!queueOccupied && !discovering &&
      mode == Fanet::ReceiveMode::Waiting) {
    if (!Fanet::mayIdleSleep(millis(), lastRadioActivity, false,
                             receivedFlag || digitalRead(PIN_DIO1) == HIGH)) {
      enterLightSleep(LIGHT_SLEEP_MAX_US);
      return;
    }
    if (receivedFlag || digitalRead(PIN_DIO1) == HIGH) return;
    if (!radioSleeping) {
      requireRadio("standby before sleep", radio.standby());
      if (receivedFlag || digitalRead(PIN_DIO1) == HIGH) {
        receivedFlag = true;
        return;
      }
      radio.clearPacketReceivedAction();
      const int16_t result = radio.sleep();
      if (result != RADIOLIB_ERR_NONE) {
        Serial.printf("ERROR: radio sleep failed, RadioLib code=%d. Returning to continuous RX.\n",
                      result);
        receiveSchedule.reset();
        startReceiving();
        return;
      }
      radioSleeping = true;
    }
    const uint32_t sleepMs =
        waitMs < SCHEDULED_SLEEP_MAX_MS ? waitMs : SCHEDULED_SLEEP_MAX_MS;
    const uint32_t elapsedLearningMs = millis() - lastLearningStart;
    if (elapsedLearningMs >= Fanet::RELEARN_INTERVAL_MS) return;
    const uint32_t untilRelearning =
        Fanet::RELEARN_INTERVAL_MS - elapsedLearningMs;
    uint32_t boundedSleepMs =
        sleepMs < untilRelearning ? sleepMs : untilRelearning;
    enterLightSleep(static_cast<uint64_t>(boundedSleepMs) * 1000ULL);
    if (receivedFlag || digitalRead(PIN_DIO1) == HIGH) {
      receiveSchedule.reset();
      startReceiving();
    }
    return;
  }
  if (radioSleeping) {
    startReceiving();
  }
#endif

  if (!Fanet::mayIdleSleep(millis(), lastRadioActivity, queueOccupied,
                           receivedFlag || digitalRead(PIN_DIO1) == HIGH)) {
    return;
  }
  enterLightSleep(LIGHT_SLEEP_MAX_US);
}
#endif  // ENABLE_LIGHT_SLEEP

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_AWAKE_LED, OUTPUT);
  digitalWrite(PIN_AWAKE_LED, HIGH);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, HIGH);
  Serial.begin(115200);
  delay(1200);
  Serial.println("\nFANET weather/name repeater - Heltec WiFi LoRa 32 V3");
  Serial.println("External LED GPIO 4: ON = ESP32 awake, OFF = light sleep.");
  initializeWatchdog();
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  const bool restoredSchedule = loadPersistentSchedule();
  requireRadio("begin", radio.begin(FREQUENCY_MHZ, BANDWIDTH_KHZ, SPREADING_FACTOR,
                                    CODING_RATE, SYNC_WORD, TX_POWER_DBM,
                                    PREAMBLE_SYMBOLS, 1.8f));
  requireRadio("DIO2 RF switch", radio.setDio2AsRfSwitch(true));
  requireRadio("DCDC regulator", radio.setRegulatorDCDC());
  requireRadio("CRC", radio.setCRC(2));
  initLightSleep();
  startReceiving();
  Serial.println("RX ready. Only types 0x04/0x02 WITH forward=1 are relayed.");
#if ENABLE_LIGHT_SLEEP
#if ENABLE_ADAPTIVE_RX
  Serial.println("Adaptive RX enabled: learning forwarded weather/name cadence before radio sleep.");
#else
  Serial.println("Adaptive RX disabled: radio remains in continuous receive.");
#endif
#else
  Serial.println("Normal light sleep disabled; ESP32 remains awake.");
#endif
  lastRadioActivity = millis();
  lastLearningStart = millis();
  if (restoredSchedule) {
    // Known stations: control only, no full discovery unless needed.
    controlActive = true;
    receiveSchedule.beginControl(lastLearningStart);
    Serial.println("Restart control: continuous RX for at least 6 minutes; "
                   "unheard stored stations will be deleted.");
  } else {
    beginLearning(lastLearningStart);
  }
}

void loop() {
#if ENABLE_LIGHT_SLEEP && ENABLE_ADAPTIVE_RX
  const uint32_t now = millis();
  if (Fanet::relearningDue(now, lastLearningStart)) {
    beginLearning(now);
    if (radioSleeping) startReceiving();
    Serial.println("12-hour station discovery: continuous RX for at least "
                   "6 minutes; stations not heard are deleted.");
  }
#endif
  receivePacket();
  forwardPacket();
  if (uint32_t(millis() - lastStatus) >= STATUS_INTERVAL_MS) {
    lastStatus = millis();
    Serial.printf("Status: RX=%lu TX=%lu filtered=%lu duplicates=%lu dropped=%lu "
                  "cadence=%u/%u\n",
                  static_cast<unsigned long>(receivedCount),
                  static_cast<unsigned long>(forwardedCount),
                  static_cast<unsigned long>(filteredCount),
                  static_cast<unsigned long>(duplicateCount),
                  static_cast<unsigned long>(droppedCount),
                  static_cast<unsigned>(receiveSchedule.learnedCount()),
                  static_cast<unsigned>(receiveSchedule.trackedCount()));
  }
#if ENABLE_LIGHT_SLEEP
  idleLightSleep();
#endif
  const esp_err_t watchdog = esp_task_wdt_reset();
  if (watchdog != ESP_OK) {
    watchdogSetupFailed(watchdog);
  }
  delay(2);
}

#endif  // FANET_PROTOCOL_TEST
