#pragma once

#include "Config.h"

class Scope {
 public:
  void arm() {
    if (state == Idle) state = Capturing;
  }

  void capture(const int16_t *samples, size_t count) {
    if (state != Capturing) return;
    size_t n = min(count, (size_t)(SCOPE_SAMPLES - filled));
    memcpy(buffer + filled, samples, n * sizeof(int16_t));
    filled += n;
    if (filled == SCOPE_SAMPLES) state = Ready;
  }

  void printIfReady(const char *source) {
    if (state != Ready) return;
    String encoded = base64::encode((uint8_t *)buffer, sizeof(buffer));
    Serial.printf("{\"type\":\"wave\",\"src\":\"%s\",\"rate\":%d,\"data\":\"%s\"}\n", source,
                  (int)info.sample_rate, encoded.c_str());
    filled = 0;
    state = Idle;
  }

 private:
  enum State { Idle, Capturing, Ready };
  volatile State state = Idle;
  int16_t buffer[SCOPE_SAMPLES];
  size_t filled = 0;
};

Scope txScope;
Scope rxScope;
volatile bool scopeEnabled = true;

void updateScopes() {
  static uint32_t lastArm = 0;
  if (!scopeEnabled) return;
  txScope.printIfReady("tx");
  rxScope.printIfReady("rx");
  if (millis() - lastArm < SCOPE_INTERVAL_MS) return;
  lastArm = millis();
  txScope.arm();
  rxScope.arm();
}
