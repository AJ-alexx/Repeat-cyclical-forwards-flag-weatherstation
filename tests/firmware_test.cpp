#include <cassert>
#include <iostream>

#include "../geplantemasterrepeat/geplantemasterrepeat.ino"

void resetFirmware() {
  Mock::now = 0;
  for (int& pin : Mock::pins) pin = LOW;
  Mock::pins[PIN_LED_INHIBIT] = HIGH;
  Mock::pins[PIN_EXTERNAL_RESET] = HIGH;
  Mock::radioWakeup = false;
  Mock::gpioWakeup = false;
  Mock::gpioWakeupPin = -1;
  Mock::gpioWakeupLevel = -1;
  Mock::sleepDurationUs = 0;
  Mock::batteryAdcMillivolts = 684;
  Mock::sleepCalls = 0;
  Mock::deepSleepCalls = 0;
  Mock::watchdogCalls = 0;
  Mock::restarts = 0;
  Mock::sleepResult = ESP_OK;
  Mock::log.clear();
  MockPreferences::batteryLock = false;
  MockPreferences::batteryStorageAvailable = true;
  MockPreferences::cadenceStorageAvailable = true;
  MockPreferences::removeCalls = 0;
  MockPreferences::removeSucceeds = true;
  radio.rxCalls = radio.txCalls = radio.readCalls = 0;
  radio.asleep = radio.listening = false;
  radio.callback = nullptr;
  radio.onTransmit = nullptr;
  radio.transmitResult = RADIOLIB_ERR_NONE;
  MockPreferences::bytes.clear();
  receiveSchedule.reset();
  recentPackets = Fanet::RecentPackets{};
  stationDiscovery = Fanet::StationDiscovery{};
  radioSleeping = receivedFlag = false;
  controlActive = false;
  lightSleepReady = false;
  batterySleepMode = radioInitialized = batteryRecovered = false;
  batteryStorageReady = false;
  scheduleStorageReady = false;
  lastBatteryCheck = 0;
  for (PendingPacket& packet : pending) packet = PendingPacket{};
  receivedCount = forwardedCount = filteredCount = duplicateCount = droppedCount = 0;
  lastTxFinished = txOffTimeMs = lastStatus = 0;
  lastRadioActivity = lastLearningStart = 0;
  externalResetLatch = 0;
  externalResetStartup = 0;
  emptyStartBlinkActive = false;
  emptyStartBlinkStarted = 0;
}

void rebootFirmware() {
  const uint32_t latch = externalResetLatch;
  const uint32_t startup = externalResetStartup;
  const int resetPin = Mock::pins[PIN_EXTERNAL_RESET];
  const auto bytes = MockPreferences::bytes;
  const bool storageAvailable = MockPreferences::cadenceStorageAvailable;
  const bool removeSucceeds = MockPreferences::removeSucceeds;
  const unsigned removes = MockPreferences::removeCalls;
  const unsigned restarts = Mock::restarts;
  resetFirmware();
  externalResetLatch = latch;
  externalResetStartup = startup;
  Mock::pins[PIN_EXTERNAL_RESET] = resetPin;
  MockPreferences::bytes = bytes;
  MockPreferences::cadenceStorageAvailable = storageAvailable;
  MockPreferences::removeSucceeds = removeSucceeds;
  MockPreferences::removeCalls = removes;
  Mock::restarts = restarts;
  setup();
}

void finishStartupBlink() {
  Mock::now = emptyStartBlinkStarted + EMPTY_START_BLINK_MS;
  loop();
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

void testBatteryCutoffEntersPersistentDeepSleep() {
  resetFirmware();
  Mock::batteryAdcMillivolts = 579;  // 3.30 V with the 5.7:1 divider.
  setup();
  assert(Mock::deepSleepCalls == 1);
  assert(Mock::sleepDurationUs == BATTERY_SLEEP_INTERVAL_US);
  assert(MockPreferences::batteryLock);
  assert(radio.rxCalls == 0);
  assert(Mock::log.find("Battery lockout") != std::string::npos);
}

void testBatteryRecoveryRequiresMoreThanResumeVoltage() {
  resetFirmware();
  MockPreferences::batteryLock = true;
  Mock::batteryAdcMillivolts = 631;  // About 3.60 V: remain locked.
  setup();
  assert(Mock::deepSleepCalls == 1);
  assert(MockPreferences::batteryLock);
  assert(radio.rxCalls == 0);

  resetFirmware();
  MockPreferences::batteryLock = true;
  Mock::batteryAdcMillivolts = 632;  // About 3.61 V: unlock and learn.
  setup();
  assert(Mock::deepSleepCalls == 0);
  assert(!MockPreferences::batteryLock);
  assert(batteryRecovered);
  assert(radio.listening);
  assert(stationDiscovery.active(millis(), receiveSchedule.complete()));
}

void testBatteryMonitorCutsOffDuringOperation() {
  resetFirmware();
  setup();
  assert(radio.listening);
  Mock::batteryAdcMillivolts = 579;
  Mock::now += BATTERY_CHECK_INTERVAL_MS;
  loop();
  assert(Mock::deepSleepCalls == 1);
  assert(MockPreferences::batteryLock);
  assert(radio.asleep && !radio.listening);
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
  for (uint32_t t = 30000; t < 20000 + 3600000; t += 10000) {
    schedule.observe(name, sizeof(name), t);
    assert(schedule.mode(t + 1001, waitMs) == Fanet::ReceiveMode::Learning);
  }
  assert(schedule.learnedCount() == 1);
  assert(schedule.trackedCount() == 2);
}

void testThreeMissedNameWindowsDeleteWholeStation() {
  Fanet::ReceiveSchedule schedule;
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    8, 8, 8, 0x01};
  schedule.observe(name, sizeof(name), 0);
  schedule.observe(name, sizeof(name), 10000);
  schedule.observe(name, sizeof(name), 20000);
  assert(schedule.learnedCount() == 1);

  uint32_t waitMs = 0;
  schedule.mode(31001, waitMs);
  schedule.mode(42001, waitMs);
  schedule.mode(54001, waitMs);
  assert(schedule.trackedCount() == 0);
  assert(schedule.persistenceDirty());
  assert(schedule.observe(name, sizeof(name), 60000));
  assert(schedule.trackedCount() == 2);
}

void testTenMissedWeatherWindowsDisableAndCanBeRelearned() {
  Fanet::ReceiveSchedule schedule;
  uint8_t weather[] = {static_cast<uint8_t>(Fanet::WEATHER | Fanet::FORWARD_FLAG),
                       9, 9, 9, 0, 0, 0, 0, 0, 0, 0};
  for (uint32_t t = 0; t <= 30000; t += 10000) {
    schedule.observe(weather, sizeof(weather), t);
  }
  assert(schedule.learnedCount() == 1);

  uint32_t waitMs = 0;
  schedule.mode(41001, waitMs, true);
  schedule.mode(52001, waitMs, true);
  schedule.mode(64001, waitMs, true);
  schedule.mode(64974, waitMs, true);
  assert(waitMs == 1);

  for (uint32_t t = 74976; t <= 134976; t += 10000) {
    schedule.mode(t, waitMs, true);
  }
  Fanet::PersistedSchedule saved[Fanet::MAX_SCHEDULES];
  assert(schedule.exportSchedules(saved, Fanet::MAX_SCHEDULES) == 1);
  assert(saved[0].type == Fanet::WEATHER);
  assert(saved[0].searchDisabled);

  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    9, 9, 9, 0x01};
  assert(schedule.observe(name, sizeof(name), 140000));
  assert(schedule.exportSchedules(saved, Fanet::MAX_SCHEDULES) == 1);
  assert(saved[0].searchDisabled);

  assert(schedule.observe(weather, sizeof(weather), 150000));
  assert(schedule.exportSchedules(saved, Fanet::MAX_SCHEDULES) == 0);
}

void testNameSchedulePersistsWithoutWeatherSchedule() {
  resetFirmware();
  assert(!loadPersistentSchedule());
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    12, 12, 12, 0x01};
  receiveSchedule.observe(name, sizeof(name), 1000);
  receiveSchedule.observe(name, sizeof(name), 11000);
  receiveSchedule.observe(name, sizeof(name), 21000);
  persistScheduleIfNeeded();

  receiveSchedule.reset();
  assert(loadPersistentSchedule());
  Fanet::PersistedSchedule saved[Fanet::MAX_SCHEDULES];
  assert(receiveSchedule.exportSchedules(saved, Fanet::MAX_SCHEDULES) == 1);
  assert(saved[0].type == Fanet::NAME);
}

void testUnknownForwardedWeatherRestartsLearning() {
  Fanet::ReceiveSchedule schedule;
  uint8_t name[] = {static_cast<uint8_t>(Fanet::NAME | Fanet::FORWARD_FLAG),
                    13, 13, 13, 0x01};
  uint8_t weather[] = {static_cast<uint8_t>(Fanet::WEATHER | Fanet::FORWARD_FLAG),
                       13, 13, 13, 0, 0, 0, 0, 0, 0, 0};
  assert(schedule.observe(name, sizeof(name), 0));
  assert(!schedule.observe(name, sizeof(name), 10000));
  assert(!schedule.observe(name, sizeof(name), 20000));
  assert(schedule.observe(weather, sizeof(weather), 21000));
}

void testDisabledWeatherPersistsAndLegacyStorageLoads() {
  resetFirmware();
  ScheduleStorageImage image;
  image.count = 1;
  image.schedules[0] = storedSchedule(10, Fanet::WEATHER, 60000);
  image.schedules[0].searchDisabled = 1;
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&image);
  MockPreferences::bytes.assign(raw, raw + sizeof(image));
  assert(loadPersistentSchedule());
  Fanet::PersistedSchedule exported[Fanet::MAX_SCHEDULES];
  assert(receiveSchedule.exportSchedules(exported, Fanet::MAX_SCHEDULES) == 1);
  assert(exported[0].searchDisabled == 1);

  resetFirmware();
  ScheduleStorageImageV1 oldImage;
  oldImage.count = 1;
  oldImage.schedules[0].address[0] = 11;
  oldImage.schedules[0].address[1] = 11;
  oldImage.schedules[0].address[2] = 11;
  oldImage.schedules[0].type = Fanet::NAME;
  oldImage.schedules[0].period = 10000;
  const uint8_t* oldRaw = reinterpret_cast<const uint8_t*>(&oldImage);
  MockPreferences::bytes.assign(oldRaw, oldRaw + sizeof(oldImage));
  assert(loadPersistentSchedule());
  assert(receiveSchedule.trackedCount() == 1);
  assert(!receiveSchedule.complete());
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
  assert(Mock::log.find("Battery:") != std::string::npos);
  assert(lightSleepReady && Mock::radioWakeup);
  finishStartupBlink();
  assert(Mock::pins[PIN_TX_LED] == LOW);
  assert(Mock::pins[PIN_AWAKE_LED] == HIGH);
  radio.onTransmit = []() {
    assert(Mock::pins[PIN_TX_LED] == HIGH);
    assert(Mock::pins[PIN_LED] == HIGH);
  };

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
  assert(Mock::pins[PIN_TX_LED] == LOW);

  const unsigned sleepCalls = Mock::sleepCalls;
  enterLightSleep(1000);
  assert(Mock::sleepCalls == sleepCalls + 1 && Mock::radioWakeup);
  assert(Mock::pins[PIN_TX_LED] == LOW);
}

void testGroundedInputDisablesBothExternalLeds() {
  resetFirmware();
  Mock::pins[PIN_LED_INHIBIT] = LOW;
  setup();
  assert(Mock::pins[PIN_AWAKE_LED] == LOW);
  assert(Mock::pins[PIN_TX_LED] == LOW);

  Mock::pins[PIN_LED_INHIBIT] = HIGH;
  loop();
  assert(Mock::pins[PIN_AWAKE_LED] == HIGH);

  Mock::pins[PIN_LED_INHIBIT] = LOW;
  loop();
  assert(Mock::pins[PIN_AWAKE_LED] == LOW);

  pending[0].data[0] = Fanet::NAME;
  pending[0].length = 4;
  pending[0].queuedAt = millis();
  pending[0].attemptAt = millis();
  pending[0].waitMs = 0;
  radio.onTransmit = []() {
    assert(Mock::pins[PIN_TX_LED] == LOW);
  };
  forwardPacket();
  assert(radio.txCalls == 1);
  assert(Mock::pins[PIN_TX_LED] == LOW);
}

void testExternalResetTriggersOnceUntilReleased() {
  resetFirmware();
  MockPreferences::bytes = {1, 2, 3};
  Mock::pins[PIN_EXTERNAL_RESET] = LOW;
  setup();
  assert(Mock::restarts == 1);
  assert(MockPreferences::bytes.empty());
  assert(MockPreferences::removeCalls == 1);
  assert(radio.rxCalls == 0);

  rebootFirmware();
  assert(Mock::restarts == 1);
  assert(MockPreferences::removeCalls == 1);
  assert(radio.listening);
  assert(receiveSchedule.trackedCount() == 0);
  assert(!controlActive || stationDiscovery.active(millis(), false));
  assert(emptyStartBlinkActive);
  finishStartupBlink();
  MockPreferences::bytes = {7, 8, 9};
  for (unsigned i = 0; i < 100; ++i) loop();
  assert(MockPreferences::bytes.size() == 3);
  assert(Mock::gpioWakeupLevel == GPIO_INTR_HIGH_LEVEL);
  loop();
  assert(Mock::restarts == 1);

  Mock::pins[PIN_EXTERNAL_RESET] = HIGH;
  loop();
  assert(Mock::gpioWakeupLevel == GPIO_INTR_LOW_LEVEL);
  MockPreferences::bytes = {4, 5, 6};
  Mock::pins[PIN_EXTERNAL_RESET] = LOW;
  loop();
  assert(Mock::restarts == 2);
  assert(MockPreferences::bytes.empty());
  assert(MockPreferences::removeCalls == 2);
}

void testExternalResetContinuesIfStationStorageCannotBeOpened() {
  resetFirmware();
  MockPreferences::bytes = {1, 2, 3};
  MockPreferences::cadenceStorageAvailable = false;
  Mock::pins[PIN_EXTERNAL_RESET] = LOW;

  setup();

  assert(Mock::restarts == 1);
  assert(MockPreferences::bytes.size() == 3);
  assert(Mock::log.find("Stored station schedules retained") != std::string::npos);
  assert(Mock::log.find("ERROR:") == std::string::npos);
  rebootFirmware();
  assert(Mock::restarts == 1);
  assert(radio.listening);
  assert(receiveSchedule.trackedCount() == 0);
  assert(!scheduleStorageReady);
  assert(!emptyStartBlinkActive);
  for (unsigned i = 0; i < 100; ++i) loop();
  assert(Mock::restarts == 1);
}

void testExternalResetContinuesIfRemovalFails() {
  resetFirmware();
  ScheduleStorageImage image;
  image.count = 2;
  image.schedules[0] = storedSchedule(1, Fanet::NAME, 10000);
  image.schedules[1] = storedSchedule(1, Fanet::WEATHER, 60000);
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&image);
  MockPreferences::bytes.assign(raw, raw + sizeof(image));
  MockPreferences::removeSucceeds = false;
  Mock::pins[PIN_EXTERNAL_RESET] = LOW;
  setup();
  assert(Mock::restarts == 1);
  assert(MockPreferences::removeCalls == 1);
  rebootFirmware();
  assert(Mock::restarts == 1);
  assert(MockPreferences::removeCalls == 1);
  assert(MockPreferences::bytes.size() == sizeof(image));
  assert(radio.listening);
  assert(receiveSchedule.trackedCount() == 2);
  assert(!receiveSchedule.complete());
  assert(controlActive);
  assert(scheduleStorageReady);
  assert(Mock::log.find("Restart control:") != std::string::npos);
  assert(Mock::log.find("ERROR:") == std::string::npos);
  assert(!emptyStartBlinkActive);
  for (unsigned i = 0; i < 100; ++i) loop();
  assert(MockPreferences::removeCalls == 1);
  assert(Mock::restarts == 1);
  rebootFirmware();
  assert(Mock::restarts == 1);
  assert(receiveSchedule.trackedCount() == 2);
  assert(controlActive);
  assert(scheduleStorageReady);
  assert(!emptyStartBlinkActive);
}

void testExternalResetDoesNotRetryClearingAtBoot() {
  resetFirmware();
  MockPreferences::bytes = {1, 2, 3};
  MockPreferences::removeSucceeds = false;
  Mock::pins[PIN_EXTERNAL_RESET] = LOW;
  setup();
  assert(Mock::restarts == 1);
  MockPreferences::removeSucceeds = true;
  rebootFirmware();
  assert(Mock::restarts == 1);
  assert(MockPreferences::removeCalls == 1);
  assert(MockPreferences::bytes.size() == 3);
  assert(scheduleStorageReady);
  assert(!emptyStartBlinkActive);
  assert(radio.listening);
}

void testFailedClearingDoesNotBlinkEvenIfStorageIsEmptyAtBoot() {
  resetFirmware();
  MockPreferences::cadenceStorageAvailable = false;
  Mock::pins[PIN_EXTERNAL_RESET] = LOW;
  setup();
  assert(Mock::restarts == 1);
  MockPreferences::cadenceStorageAvailable = true;
  rebootFirmware();
  assert(MockPreferences::bytes.empty());
  assert(!emptyStartBlinkActive);
  assert(scheduleStorageReady);
  assert(radio.listening);
}

void testEmptyStartupBlinksWithoutBlocking() {
  resetFirmware();
  setup();
  assert(emptyStartBlinkActive);
  const uint32_t started = emptyStartBlinkStarted;
  const unsigned watchdogCalls = Mock::watchdogCalls;
  for (unsigned i = 0; i < 16; ++i) {
    Mock::now = started + i * 250;
    loop();
    assert(Mock::pins[PIN_AWAKE_LED] == (i % 2 == 0 ? HIGH : LOW));
    assert(Mock::pins[PIN_TX_LED] == Mock::pins[PIN_AWAKE_LED]);
    assert(Mock::now == started + i * 250 + 2);
    assert(Mock::sleepCalls == 0);
  }
  assert(Mock::watchdogCalls == watchdogCalls + 16);
  finishStartupBlink();
  assert(!emptyStartBlinkActive);
  assert(Mock::pins[PIN_AWAKE_LED] == HIGH);
  assert(Mock::pins[PIN_TX_LED] == LOW);
  assert(Mock::sleepCalls > 0);
}

void testStoredStartupDoesNotBlink() {
  resetFirmware();
  ScheduleStorageImage image;
  image.count = 1;
  image.schedules[0] = storedSchedule(1, Fanet::NAME, 10000);
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&image);
  MockPreferences::bytes.assign(raw, raw + sizeof(image));
  setup();
  assert(!emptyStartBlinkActive);
  assert(controlActive);
  assert(receiveSchedule.trackedCount() == 1);
}

void testFatalErrorRestartsWithoutWaitingForever() {
  resetFirmware();
  fatalError("test", -1);
  assert(Mock::restarts == 1);
  assert(Mock::now == 100);
}

void testExternalResetWakesFromLightSleep() {
  resetFirmware();
  setup();
  assert(lightSleepReady);
  assert(Mock::gpioWakeup);
  assert(Mock::gpioWakeupPin == PIN_EXTERNAL_RESET);
}

void testTxLedTurnsOffAfterTransmitError() {
  resetFirmware();
  setup();
  finishStartupBlink();
  radio.transmitResult = -1;
  radio.onTransmit = []() {
    assert(Mock::pins[PIN_TX_LED] == HIGH);
  };
  pending[0].data[0] = Fanet::NAME;
  pending[0].length = 4;
  pending[0].queuedAt = millis();
  pending[0].attemptAt = millis();
  pending[0].waitMs = 0;
  forwardPacket();
  assert(radio.txCalls == 1);
  assert(Mock::pins[PIN_TX_LED] == LOW);
  assert(forwardedCount == 0);
  assert(droppedCount == 1);
  assert(radio.listening);
  assert(Mock::log.find("DROP: TX error") != std::string::npos);
}

void testSleepFailureDisablesSleep() {
  resetFirmware();
  setup();
  finishStartupBlink();
  Mock::sleepResult = ESP_ERR_INVALID_STATE;
  enterLightSleep(1000);
  assert(!lightSleepReady);
  assert(Mock::log.find("ERROR: light sleep entry") != std::string::npos);
}

int main() {
  testBatteryCutoffEntersPersistentDeepSleep();
  testBatteryRecoveryRequiresMoreThanResumeVoltage();
  testBatteryMonitorCutsOffDuringOperation();
  testStoredScheduleVerification();
  testPersistentScheduleRoundTrip();
  testRestartControlDeletesUnheardStation();
  testRestartControlWaitsForSlowName();
  testNoDeletionDuringNormalOperation();
  testThreeMissedNameWindowsDeleteWholeStation();
  testTenMissedWeatherWindowsDisableAndCanBeRelearned();
  testNameSchedulePersistsWithoutWeatherSchedule();
  testUnknownForwardedWeatherRestartsLearning();
  testDisabledWeatherPersistsAndLegacyStorageLoads();
  testLearningPhaseDeletesUnheardStation();
  testFirmwareRestartPrunesStorage();
  testStartupAndRelay();
  testGroundedInputDisablesBothExternalLeds();
  testExternalResetTriggersOnceUntilReleased();
  testExternalResetContinuesIfStationStorageCannotBeOpened();
  testExternalResetContinuesIfRemovalFails();
  testExternalResetDoesNotRetryClearingAtBoot();
  testFailedClearingDoesNotBlinkEvenIfStorageIsEmptyAtBoot();
  testEmptyStartupBlinksWithoutBlocking();
  testStoredStartupDoesNotBlink();
  testFatalErrorRestartsWithoutWaitingForever();
  testExternalResetWakesFromLightSleep();
  testTxLedTurnsOffAfterTransmitError();
  testSleepFailureDisablesSleep();
  std::cout << "Firmware startup, relay and sleep tests passed\n";
}
