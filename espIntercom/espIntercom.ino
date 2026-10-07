#include "Console.h"

void setup() {
  // a large TX buffer keeps scope and stats prints from stalling the send loop
  Serial.setTxBufferSize(4096);
  Serial.setRxBufferSize(8192);
  Serial.begin(SERIAL_BAUD);
  // the core empties the 128-byte RX FIFO only at 120 bytes, which leaves 0.2 ms of slack at this baud rate;
  // the radio driver stalls interrupts for ~2 ms about once a second, so drain it much earlier
  Serial.setRxFIFOFull(16);
  Serial.onReceiveError(onSerialError);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
  loadSettings();
  setupAudioOutput();

  if (!setupEspNow()) {
    reply("ESP-NOW init failed");
    delay(2000);
    ESP.restart();
  }

  setupSources();
  setupVoiceEq();
  xTaskCreatePinnedToCore(mixerTask, "mixer", 8192, nullptr, 5, &mixerTaskHandle, 1);
  enableLoopWDT();
  bootMs = millis();
  chime.request(STARTUP_CHIME);
  printBoot();
}

void loop() {
  trackLoopGap();
  printStats();
  updateScopes();
  printGlitches();
  printAlerts();
  readSerial();
  applyPendingConfig();
  flushSettingsWhenIdle();
  delay(2);
}
