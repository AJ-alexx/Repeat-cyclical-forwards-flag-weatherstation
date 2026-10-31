#include <cassert>
#include <iostream>

#include "../geplantemasterrepeat/geplantemasterrepeat.ino"

void resetFirmware() {
  Mock::now = 0;
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
  receiveSchedule.reset();
  recentPackets = Fanet::RecentPackets{};
  stationDiscovery = Fanet::StationDiscovery{};
  radioSleeping = receivedFlag = false;
  lightSleepReady = false;
  for (PendingPacket& packet : pending) packet = PendingPacket{};
  receivedCount = forwardedCount = filteredCount = duplicateCount = droppedCount = 0;
  lastTxFinished = txOffTimeMs = lastStatus = 0;
  lastRadioActivity = lastLearningStart = 0;
}

void testStartupAndRelay() {
  resetFirmware();
  setup();
  assert(radio.listening);
  assert(radio.callback != nullptr);
  assert(Mock::log.find("RX ready") != std::string::npos);
  assert(Mock::log.find("BATTERY") == std::string::npos);
  assert(lightSleepReady && Mock::radioWakeup);

  pending[0].data[0] = Fanet::NAME;
  pending[0].data[1] = 1;
  pending[0].data[2] = 2;
  pending[0].data[3] = 3;
  pending[0].length = 4;
  pending[0].queuedAt = millis();
  pending[0].attemptAt = millis();
  pending[0].waitMs = 0;
  forwardPacket();
  assert(radio.txCalls == 1);
  assert(radio.listening && !radio.asleep);
  assert(forwardedCount == 1);

  enterLightSleep(1000);
  assert(Mock::sleepCalls == 1 && Mock::radioWakeup);
}

void testSleepFailureDisablesSleep() {
  resetFirmware();
  setup();
  Mock::sleepResult = ESP_ERR_INVALID_STATE;
  enterLightSleep(1000);
  assert(!lightSleepReady);
  assert(Mock::log.find("ERROR: light sleep entry") != std::string::npos);
}

int main() {
  testStartupAndRelay();
  testSleepFailureDisablesSleep();
  std::cout << "Firmware startup, relay and sleep tests passed\n";
}
