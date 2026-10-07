#pragma once

#include "Config.h"

struct EqBand {
  int16_t hz;
  int8_t db;
  uint8_t q10;
};

// measured on the 8 ohm test speaker with the Mac mic, then tuned by ear to tame harsh S sounds
const EqBand DEFAULT_EQ[EQ_BANDS] = {
    {500, -9, 12}, {1600, -5, 20}, {4000, 0, 30}, {5000, -8, 30}, {2500, 2, 20},
};

struct RateOption {
  int tenthMbps;
  wifi_phy_mode_t mode;
  wifi_phy_rate_t rate;
};

const RateOption RATES[] = {
    {10, WIFI_PHY_MODE_11B, WIFI_PHY_RATE_1M_L},   {20, WIFI_PHY_MODE_11B, WIFI_PHY_RATE_2M_S},
    {55, WIFI_PHY_MODE_11B, WIFI_PHY_RATE_5M_S},   {110, WIFI_PHY_MODE_11B, WIFI_PHY_RATE_11M_S},
    {60, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_6M},     {90, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_9M},
    {120, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_12M},   {180, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_18M},
    {240, WIFI_PHY_MODE_11G, WIFI_PHY_RATE_24M},
};

struct Settings {
  int pinBck = 14;
  int pinWs = 15;
  int pinData = 22;
  int toneHz = 0;
  int volume = 25;
  bool txEnabled = true;
  bool playEnabled = true;
  bool hostSource = false;
  int rateTenthMbps = 10;
  int lc3Kbps = DEFAULT_LC3_KBPS;
  int redundantKbps = DEFAULT_REDUNDANT_KBPS;
  bool adaptive = true;
  bool voiceEq = true;
  int redundancy = DEFAULT_REDUNDANCY;
  bool alerts = true;
  // group-wide settings carry a version and the unit that last changed them; the highest wins everywhere
  uint32_t groupVersion = 0;
  uint8_t groupOwner[6] = {0};
  EqBand eq[EQ_BANDS];
};

Preferences prefs;
Settings settings;
uint32_t settingsDirtySinceMs = 0;

bool validRate(int tenthMbps) {
  for (const RateOption &r : RATES) {
    if (r.tenthMbps == tenthMbps) return true;
  }
  return false;
}

bool validPin(int pin) { return pin >= 0 && pin <= 39; }

// values from older firmware or a corrupted NVS page fall back to defaults instead of breaking the board
void sanitizeSettings() {
  Settings defaults;
  if (!validPin(settings.pinBck) || !validPin(settings.pinWs) || !validPin(settings.pinData)) {
    settings.pinBck = defaults.pinBck;
    settings.pinWs = defaults.pinWs;
    settings.pinData = defaults.pinData;
  }
  if (settings.toneHz < 0 || settings.toneHz > 7900) settings.toneHz = defaults.toneHz;
  settings.volume = constrain(settings.volume, 0, 100);
  if (!validRate(settings.rateTenthMbps)) settings.rateTenthMbps = defaults.rateTenthMbps;
  if (settings.lc3Kbps < MIN_LC3_KBPS || settings.lc3Kbps > MAX_LC3_KBPS) settings.lc3Kbps = defaults.lc3Kbps;
  if (settings.redundantKbps < MIN_LC3_KBPS || settings.redundantKbps > settings.lc3Kbps) {
    settings.redundantKbps = min(defaults.redundantKbps, settings.lc3Kbps);
  }
  if (settings.redundancy < 0 || settings.redundancy > MAX_REDUNDANCY) settings.redundancy = defaults.redundancy;
  for (EqBand &b : settings.eq) {
    if (b.hz < 50 || b.hz > 7800 || b.db < -15 || b.db > 15 || b.q10 < 3 || b.q10 > 100) {
      memcpy(settings.eq, DEFAULT_EQ, sizeof(settings.eq));
      break;
    }
  }
}

void loadSettings() {
  prefs.begin("intercom", false);
  settings.pinBck = prefs.getInt("bck", settings.pinBck);
  settings.pinWs = prefs.getInt("ws", settings.pinWs);
  settings.pinData = prefs.getInt("data", settings.pinData);
  settings.toneHz = prefs.getInt("tone", settings.toneHz);
  settings.volume = prefs.getInt("vol", settings.volume);
  settings.txEnabled = prefs.getBool("tx", settings.txEnabled);
  settings.playEnabled = prefs.getBool("play", settings.playEnabled);
  settings.rateTenthMbps = prefs.getInt("rate2", settings.rateTenthMbps);
  settings.lc3Kbps = prefs.getInt("lc3kbps", settings.lc3Kbps);
  settings.redundantKbps = prefs.getInt("rkbps", settings.redundantKbps);
  settings.adaptive = prefs.getBool("adapt", settings.adaptive);
  settings.alerts = prefs.getBool("alerts", settings.alerts);
  settings.groupVersion = prefs.getUInt("gver", settings.groupVersion);
  prefs.getBytes("gowner", settings.groupOwner, sizeof(settings.groupOwner));
  settings.voiceEq = prefs.getBool("eq", settings.voiceEq);
  settings.redundancy = prefs.getInt("redund", settings.redundancy);
  memcpy(settings.eq, DEFAULT_EQ, sizeof(settings.eq));
  prefs.getBytes("eqBands", settings.eq, sizeof(settings.eq));
  sanitizeSettings();
}

void flushSettings() {
  prefs.putInt("bck", settings.pinBck);
  prefs.putInt("ws", settings.pinWs);
  prefs.putInt("data", settings.pinData);
  prefs.putInt("tone", settings.toneHz);
  prefs.putInt("vol", settings.volume);
  prefs.putBool("tx", settings.txEnabled);
  prefs.putBool("play", settings.playEnabled);
  prefs.putInt("rate2", settings.rateTenthMbps);
  prefs.putInt("lc3kbps", settings.lc3Kbps);
  prefs.putInt("rkbps", settings.redundantKbps);
  prefs.putBool("adapt", settings.adaptive);
  prefs.putBool("alerts", settings.alerts);
  prefs.putUInt("gver", settings.groupVersion);
  prefs.putBytes("gowner", settings.groupOwner, sizeof(settings.groupOwner));
  prefs.putBool("eq", settings.voiceEq);
  prefs.putInt("redund", settings.redundancy);
  prefs.putBytes("eqBands", settings.eq, sizeof(settings.eq));
  settingsDirtySinceMs = 0;
}

// flash writes stall both cores for a few ms, so bursts of changes are written once they settle
void markSettingsDirty() { settingsDirtySinceMs = millis() | 1; }

void flushSettingsWhenIdle() {
  if (settingsDirtySinceMs && millis() - settingsDirtySinceMs > SETTINGS_SAVE_DELAY_MS) flushSettings();
}
