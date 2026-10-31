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
  MockPreferences::bytes.clear();
  receiveSchedule.reset();
  recentPackets = Fanet::RecentPackets{};
  stationDiscovery = Fanet::StationDiscovery{};
  radioSleeping = receivedFlag = false;
  controlActive = false;
  lightSleepReady = false;
  for (PendingPacket& packet : pending) packet = PendingPacket{};
  receivedCount = forwardedCount = filteredCount = duplicateCount = droppedCount = 0;
  lastTxFinished = txOffTimeMs = lastStatus = 0;
  lastRadioActivity = lastLearningStart = 0;
}

void testStoredScheduleVerification() {
  Fanet::PersistedSchedule saved;
  saved.address[0] = 0x11;
  saved.address[1] = 0x22;
  saved.address[2] = 0x33;
  saved.type = Fanet::NAME;
  saved.period = 10000;

  Fanet::ReceiveSchedule schedule;
  assert(schedule.restore(&saved, 1));
  assert(!schedule.complete());
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    0x11, 0x22, 0x33, 0x01};
  assert(!schedule.observe(name, sizeof(name), 1000));
  assert(!schedule.complete());
  assert(!schedule.observe(name, sizeof(name), 11000));
  assert(schedule.complete());
  Fanet::PersistedSchedule exported[Fanet::MAX_SCHEDULES];
  assert(schedule.exportSchedules(exported, Fanet::MAX_SCHEDULES) == 1);
  assert(exported[0].period == 10000);

  Fanet::ReceiveSchedule changed;
  assert(changed.restore(&saved, 1));
  assert(!changed.observe(name, sizeof(name), 1000));
  assert(!changed.observe(name, sizeof(name), 16000));
  assert(changed.takeRelearningDetected());
  assert(!changed.complete());
  assert(changed.exportSchedules(exported, Fanet::MAX_SCHEDULES) == 0);

  uint8_t weather[] = {static_cast<uint8_t>(Fanet::WEATHER | Fanet::FORWARD_FLAG),
                       0x11, 0x22, 0x33, 0, 0, 0, 0, 0, 0, 0};
  assert(changed.observe(weather, sizeof(weather), 20000));
  assert(!changed.observe(name, sizeof(name), 31000));
  assert(changed.trackedCount() == 2);
  assert(changed.exportSchedules(exported, Fanet::MAX_SCHEDULES) == 1);
  assert(exported[0].period == 15000);
}

void testPersistentScheduleRoundTrip() {
  resetFirmware();
  assert(!loadPersistentSchedule());
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    0x44, 0x55, 0x66, 0x01};
  receiveSchedule.observe(name, sizeof(name), 1000);
  receiveSchedule.observe(name, sizeof(name), 11000);
  receiveSchedule.observe(name, sizeof(name), 21000);
  assert(receiveSchedule.persistenceDirty());
  persistScheduleIfNeeded();
  assert(!receiveSchedule.persistenceDirty());

  receiveSchedule.reset();
  assert(loadPersistentSchedule());
  assert(receiveSchedule.trackedCount() == 1);
  assert(!receiveSchedule.complete());
  receiveSchedule.observe(name, sizeof(name), 30000);
  receiveSchedule.observe(name, sizeof(name), 40000);
  assert(receiveSchedule.complete());
}

Fanet::PersistedSchedule storedSchedule(uint8_t id, uint8_t type,
                                        uint32_t period) {
  Fanet::PersistedSchedule schedule;
  schedule.address[0] = id;
  schedule.address[1] = id;
  schedule.address[2] = id;
  schedule.type = type;
  schedule.period = period;
  return schedule;
}

void testRestartControlDeletesUnheardStation() {
  const Fanet::PersistedSchedule saved[] = {
      storedSchedule(1, Fanet::WEATHER, 60000),
      storedSchedule(1, Fanet::NAME, 120000),
      storedSchedule(2, Fanet::WEATHER, 60000)};
  Fanet::ReceiveSchedule schedule;
  assert(schedule.restore(saved, 3));
  schedule.beginControl(0);

  uint8_t weather[] = {static_cast<uint8_t>(Fanet::WEATHER | Fanet::FORWARD_FLAG),
                       1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    1, 1, 1, 0x01};
  for (uint32_t t = 10000; t < 360000; t += 60000) {
    schedule.observe(weather, sizeof(weather), t);
  }
  schedule.observe(name, sizeof(name), 30000);
  schedule.observe(name, sizeof(name), 150000);

  assert(schedule.controlPending(300000));
  assert(schedule.controlPending(359999));
  assert(!schedule.controlPending(360000));
  assert(schedule.pruneUnheardStations() == 1);
  assert(schedule.trackedCount() == 2);
  assert(!schedule.hasUnverifiedRestored());
  assert(schedule.complete());
}

void testRestartControlWaitsForSlowName() {
  const Fanet::PersistedSchedule saved[] = {
      storedSchedule(1, Fanet::WEATHER, 60000),
      storedSchedule(1, Fanet::NAME, 300000)};
  Fanet::ReceiveSchedule schedule;
  assert(schedule.restore(saved, 2));
  schedule.beginControl(0);
  uint8_t weather[] = {static_cast<uint8_t>(Fanet::WEATHER | Fanet::FORWARD_FLAG),
                       1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  schedule.observe(weather, sizeof(weather), 10000);
  schedule.observe(weather, sizeof(weather), 70000);
  // The station is present, so its NAME needs time for two packets.
  assert(schedule.controlPending(400000));
  assert(!schedule.controlPending(700000));
  assert(schedule.hasUnverifiedRestored());
}

void testNoDeletionDuringNormalOperation() {
  Fanet::ReceiveSchedule schedule;
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    7, 7, 7, 0x01};
  schedule.observe(name, sizeof(name), 0);
  schedule.observe(name, sizeof(name), 10000);
  schedule.observe(name, sizeof(name), 20000);
  assert(schedule.learnedCount() == 1);
  uint32_t waitMs = 0;
  for (uint32_t t = 20000; t < 20000 + 3600000; t += 1000) {
    schedule.mode(t, waitMs, true);
  }
  assert(schedule.learnedCount() == 1);
  assert(schedule.trackedCount() == 2);
}

void testLearningPhaseDeletesUnheardStation() {
  Fanet::ReceiveSchedule schedule;
  uint8_t a[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                 1, 1, 1, 0x01};
  uint8_t b[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                 2, 2, 2, 0x01};
  for (uint32_t t = 0; t <= 20000; t += 10000) {
    schedule.observe(a, sizeof(a), t);
    schedule.observe(b, sizeof(b), t + 1);
  }
  assert(schedule.learnedCount() == 2);
  schedule.beginControl(100000);
  for (uint32_t t = 100000; t < 100000 + Fanet::CONTROL_MIN_MS; t += 10000) {
    schedule.observe(a, sizeof(a), t);
  }
  assert(schedule.controlPending(100000 + Fanet::CONTROL_MIN_MS - 1));
  assert(!schedule.controlPending(100000 + Fanet::CONTROL_MIN_MS));
  assert(schedule.pruneUnheardStations() == 1);
  assert(schedule.learnedCount() == 1);
  assert(schedule.trackedCount() == 2);
}

void testFirmwareRestartPrunesStorage() {
  resetFirmware();
  ScheduleStorageImage image;
  image.count = 1;
  image.schedules[0] = storedSchedule(3, Fanet::WEATHER, 60000);
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&image);
  MockPreferences::bytes.assign(raw, raw + sizeof(image));
  setup();
  assert(controlActive);
  while (Mock::now < 5UL * 60UL * 1000UL) {
    Mock::now += 100;
    idleLightSleep();
  }
  assert(controlActive);
  assert(MockPreferences::bytes.size() == sizeof(image));
  while (Mock::now < 8UL * 60UL * 1000UL && controlActive) {
    Mock::now += 100;
    idleLightSleep();
  }
  assert(!controlActive);
  ScheduleStorageImage stored;
  memcpy(&stored, MockPreferences::bytes.data(), sizeof(stored));
  assert(stored.count == 0);
  assert(Mock::log.find("1 unheard station(s) deleted") != std::string::npos);
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
  testStoredScheduleVerification();
  testPersistentScheduleRoundTrip();
  testRestartControlDeletesUnheardStation();
  testRestartControlWaitsForSlowName();
  testNoDeletionDuringNormalOperation();
  testLearningPhaseDeletesUnheardStation();
  testFirmwareRestartPrunesStorage();
  testStartupAndRelay();
  testSleepFailureDisablesSleep();
  std::cout << "Firmware startup, relay and sleep tests passed\n";
}
