#include <cassert>
#include <cmath>
#include <iostream>

#include "../geplantemasterrepeat/geplantemasterrepeat.ino"

void resetFirmware() {
  Mock::now = 0;
  Mock::adcMillivolts = 800;
  for (int& pin : Mock::pins) pin = LOW;
  Mock::radioWakeup = false;
  Mock::sleepCalls = 0;
  Mock::watchdogCalls = 0;
  Mock::restarts = 0;
  Mock::sleepResult = ESP_OK;
  Mock::log.clear();
  radio.rxCalls = radio.txCalls = radio.readCalls = 0;
  radio.asleep = radio.listening = false;
  radio.callback = nullptr;
  batteryProtection = Fanet::BatteryProtection{};
  receiveSchedule.reset();
  recentPackets = Fanet::RecentPackets{};
  stationDiscovery = Fanet::StationDiscovery{};
  radioSleeping = receivedFlag = false;
  lightSleepReady = false;
  radioWakeupEnabled = false;
  for (PendingPacket& packet : pending) packet = PendingPacket{};
  receivedCount = forwardedCount = filteredCount = duplicateCount = droppedCount = 0;
  lastTxFinished = txOffTimeMs = lastBatteryCheck = lastStatus = 0;
  lastRadioActivity = lastLearningStart = 0;
}

void testThresholds() {
  Fanet::BatteryProtection protection;
  assert(!protection.update(3.30f));
  assert(!protection.suspended());
  assert(protection.update(std::nextafter(3.30f, 0.0f)));
  assert(protection.suspended());
  for (float voltage : {3.30f, 3.45f, 3.60f, NAN}) {
    assert(!protection.update(voltage));
    assert(protection.suspended());
  }
  assert(protection.update(std::nextafter(3.60f, 4.0f)));
  assert(!protection.suspended());
  assert(!protection.update(3.45f));
}

void testShutdownAndRecovery() {
  resetFirmware();
  setup();
  assert(radio.listening);
  const uint8_t name[] = {0x42, 1, 2, 3, 'A'};
  for (uint32_t now : {10000U, 20000U, 30000U}) {
    receiveSchedule.observe(name, sizeof(name), now);
  }
  assert(receiveSchedule.learnedCount() == 1);
  recentPackets.remember(name, sizeof(name), millis());
  pending[0].length = sizeof(name);
  pending[1].length = sizeof(name);
  receivedFlag = true;
  Mock::pins[PIN_DIO1] = HIGH;
  Mock::adcMillivolts = 673;  // 3.2977 V after the configured divider.
  Mock::now = lastBatteryCheck + BATTERY_INTERVAL_MS;
  const unsigned rxBefore = radio.rxCalls;
  loop();
  assert(batteryProtection.suspended());
  assert(radio.asleep && !radio.listening && radio.callback == nullptr);
  assert(!receivedFlag && pending[0].length == 0 && pending[1].length == 0);
  assert(droppedCount == 2);
  assert(radio.rxCalls == rxBefore && radio.txCalls == 0 && radio.readCalls == 0);
  assert(Mock::sleepCalls == 1 && !Mock::radioWakeup);
  assert(Mock::sleepDurationUs <= uint64_t(BATTERY_INTERVAL_MS) * 1000);
  assert(Mock::watchdogCalls > 1 && Mock::restarts == 0);
  startReceiving();
  receivePacket();
  forwardPacket();
  assert(radio.rxCalls == rxBefore && radio.txCalls == 0 && radio.readCalls == 0);

  for (uint32_t adc : {674U, 734U, 0U}) {
    Mock::adcMillivolts = adc;
    const unsigned sleepsBefore = Mock::sleepCalls;
    loop();
    assert(batteryProtection.suspended());
    assert(radio.rxCalls == rxBefore && radio.asleep);
    assert(lightSleepReady && Mock::sleepCalls == sleepsBefore + 1);
  }
  Mock::now = lastBatteryCheck + Fanet::RELEARN_INTERVAL_MS;
  loop();
  assert(radio.rxCalls == rxBefore && radio.asleep);

  Mock::adcMillivolts = 735;  // 3.6015 V: recovery is strictly above 3.60 V.
  Mock::pins[PIN_DIO1] = LOW;
  loop();
  assert(!batteryProtection.suspended() && radio.listening && !radioSleeping);
  assert(receiveSchedule.trackedCount() == 0);
  assert(!recentPackets.contains(name, sizeof(name), millis()));
  assert(stationDiscovery.active(millis(), receiveSchedule.complete()));
  assert(uint32_t(millis() - lastLearningStart) < 1000);
  assert(Mock::log.find("BATTERY RECOVERED") != std::string::npos);
  enterLightSleep(1000);
  assert(Mock::radioWakeup && radioWakeupEnabled && lightSleepReady);
}

void testLowBatteryStartup() {
  resetFirmware();
  Mock::adcMillivolts = 673;
  setup();
  assert(batteryProtection.suspended());
  assert(radio.rxCalls == 0 && radio.asleep);
  assert(Mock::log.find("RX ready") == std::string::npos);
  loop();
  assert(Mock::sleepCalls == 1 && !Mock::radioWakeup);
}

void testSleepFailureRemainsProtected() {
  resetFirmware();
  Mock::adcMillivolts = 673;
  setup();
  Mock::sleepResult = ESP_ERR_INVALID_STATE;
  loop();
  assert(!lightSleepReady);
  loop();
  assert(batteryProtection.suspended() && radio.asleep);
  assert(radio.rxCalls == 0 && radio.txCalls == 0);
  assert(Mock::log.find("ERROR: light sleep entry") != std::string::npos);
}

int main() {
  testThresholds();
  testShutdownAndRecovery();
  testLowBatteryStartup();
  testSleepFailureRemainsProtected();
  std::cout << "Battery thresholds and firmware integration passed\n";
}
