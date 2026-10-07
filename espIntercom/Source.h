#pragma once

#include "Transport.h"

SineWaveGenerator<int16_t> sineWave(SINE_AMPLITUDE);
StreamBufferHandle_t hostStream;
TaskHandle_t txTaskHandle;
esp_timer_handle_t txTimer;
esp_timer_handle_t slotShiftTimer;
volatile uint32_t slotMoves = 0;
volatile uint8_t relayingCount = 0;
volatile bool encoderReconfigurePending = false;
volatile bool hostResetPending = false;
volatile uint32_t hostUnderruns = 0;
volatile uint32_t hostDriftDrops = 0;
volatile uint32_t hostDriftInserts = 0;
volatile uint32_t hostOverflowBytes = 0;
volatile uint32_t hostBytes = 0;
volatile uint32_t txCatchUpFrames = 0;
volatile uint32_t encodeErrors = 0;
volatile uint32_t encodeUsTotal = 0;
volatile uint32_t encodeCount = 0;

// the redundant copy is a second, independent encode at a lower bitrate, so it needs its own encoder state
lc3_encoder_mem_16k_t mainEncoderMemory;
lc3_encoder_mem_16k_t redundantEncoderMemory;
lc3_encoder_t mainEncoder;
lc3_encoder_t redundantEncoder;

int defaultTone() {
  static const uint16_t tones[] = {440, 554, 659, 880};
  return tones[ownMac[5] % 4];
}

int activeTone() { return settings.toneHz > 0 ? settings.toneHz : defaultTone(); }

size_t hostSamplesAvailable() { return xStreamBufferBytesAvailable(hostStream) / sizeof(int16_t); }

void selectHostSource(bool host) {
  settings.hostSource = host;
  hostResetPending = true;
}

void writeHostAudio(const uint8_t *pcm, size_t len) {
  size_t written = xStreamBufferSend(hostStream, pcm, len, 0);
  hostOverflowBytes += len - written;
  hostBytes += len;
}

// G.711 mu-law expansion, used for the host feed so the serial link carries half the bytes
int16_t ulawToLinear(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return (u & 0x80) ? 0x84 - t : t - 0x84;
}

void writeHostUlaw(const uint8_t *ulaw, size_t count) {
  static int16_t pcm[HOST_FRAME_MAX_BYTES];
  for (size_t i = 0; i < count; i++) pcm[i] = ulawToLinear(ulaw[i]);
  writeHostAudio((const uint8_t *)pcm, count * sizeof(int16_t));
}

void readHostSamples(int16_t *out, size_t count) {
  xStreamBufferReceive(hostStream, out, count * sizeof(int16_t), 0);
}

// host audio arrives over USB on the Mac's clock, so it gets its own small jitter buffer
void readHostFrame(int16_t *block) {
  static bool primed = false;
  if (hostResetPending) {
    xStreamBufferReset(hostStream);
    primed = false;
    hostResetPending = false;
  }
  size_t level = hostSamplesAvailable();
  if (!primed && level >= HOST_PRIME_SAMPLES) primed = true;
  if (!primed || level < SAMPLES_PER_FRAME) {
    if (primed) hostUnderruns++;
    primed = false;
    memset(block, 0, SAMPLES_PER_FRAME * sizeof(int16_t));
    return;
  }
  if (level < HOST_LOW_SAMPLES) {
    readHostSamples(block, SAMPLES_PER_FRAME - 1);
    block[SAMPLES_PER_FRAME - 1] = block[SAMPLES_PER_FRAME - 2];
    hostDriftInserts++;
    return;
  }
  if (level > HOST_HIGH_SAMPLES) {
    int16_t dropped;
    readHostSamples(&dropped, 1);
    hostDriftDrops++;
  }
  readHostSamples(block, SAMPLES_PER_FRAME);
}

int frameBytesFor(int kbps) { return lc3_frame_bytes(LC3_FRAME_US, kbps * 1000); }

enum LinkLevel : uint8_t { LINK_GOOD, LINK_MEDIUM, LINK_POOR };
const char *LINK_LEVEL_NAMES[] = {"good", "medium", "poor"};
const Profile LEVEL_PROFILES[] = {{48, 16, 2}, {32, 16, 2}, {16, 16, 2}};

volatile uint8_t linkLevel = LINK_MEDIUM;
volatile uint8_t adaptListeners = 0;
volatile uint8_t adaptSenders = 1;
volatile float worstListenerLossPct = 0;
volatile int8_t worstListenerRssi = 0;
volatile uint32_t profileChanges = 0;

size_t profilePacketBytes(const Profile &p) {
  return sizeof(PacketHeader) + FRAMES_PER_PACKET * (frameBytesFor(p.kbps) + p.depth * frameBytesFor(p.redundantKbps));
}

// shrinks the profile until this unit's share of the packet cycle fits next to everyone else's
Profile fitAirtime(Profile p, int senders) {
  float budget = PACKET_US * AIRTIME_BUDGET / senders;
  while (airtimeUs(profilePacketBytes(p)) + SLOT_MARGIN_US > budget) {
    if (p.kbps > 24) p.kbps -= 8;
    else if (p.depth > 1) p.depth--;
    else if (p.kbps > MIN_LC3_KBPS) p.kbps -= 8;
    else break;
  }
  return p;
}

// one-hop relay: forward an origin this unit hears well when a neighbour that hears this unit well
// reports hearing that origin badly or not at all; among willing relays the lowest MAC keeps the job
bool shouldRelay(Peer &origin, uint32_t now) {
  if (msSince(origin.lastSeenMs, now) > PEER_ACTIVE_MS) return false;
  if (origin.directLossEma > RELAY_LINK_MAX_LOSS_PCT || origin.rssiEma < RELAY_SOURCE_MIN_RSSI) return false;
  bool needed = false;
  for (Peer &n : peers) {
    if (!n.used || &n == &origin || msSince(n.reportMs, now) > REPORT_FRESH_MS) continue;
    const Peer::ReportEntry *aboutMe = findEntry(n, ownMac);
    if (!aboutMe || entryLoss(aboutMe) > RELAY_LINK_MAX_LOSS_PCT) continue;
    const Peer::ReportEntry *aboutOrigin = findEntry(n, origin.mac);
    if (aboutOrigin && (aboutOrigin->loss & REPORT_RELAY_BIT) && memcmp(n.mac, ownMac, 6) < 0) return false;
    if (!aboutOrigin || entryLoss(aboutOrigin) > RELAY_NEED_LOSS_PCT) needed = true;
  }
  return needed;
}

int updateRelays() {
  uint32_t now = millis();
  int relaying = 0;
  for (Peer &p : peers) {
    if (!p.used) continue;
    if (shouldRelay(p, now)) {
      p.relaying = true;
      p.relayHoldUntilMs = now + RELAY_HOLD_MS;
    } else if ((int32_t)(now - p.relayHoldUntilMs) > 0) {
      p.relaying = false;
    }
    if (p.relaying) relaying++;
  }
  return relaying;
}

// relayed copies go out right after this unit's own packet, inside the slot it already holds, so they
// cannot land in another sender's slot; a muted unit has no slot and forwards at once
void relayTask(void *) {
  static RelayItem item;
  while (true) {
    if (settings.txEnabled) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2 * PACKET_US / 1000));
      while (xQueueReceive(relayQueue, &item, 0) == pdTRUE) packetSink.sendRelayed(item.data, item.len, item.origin);
    } else if (xQueueReceive(relayQueue, &item, pdMS_TO_TICKS(50)) == pdTRUE) {
      packetSink.sendRelayed(item.data, item.len, item.origin);
    }
  }
}

volatile uint8_t relayServedListeners = 0;

// a listener that hears this unit only through a relay is judged by how it hears that relay, because
// the relay forwards these packets unchanged and its hop to the listener is what carries them
bool relayedQuality(const Peer &listener, uint32_t now, float &loss, int &rssi) {
  bool found = false;
  for (const Peer &relay : peers) {
    if (!relay.used || &relay == &listener || msSince(relay.reportMs, now) > REPORT_FRESH_MS) continue;
    const Peer::ReportEntry *relaysMe = findEntry(relay, ownMac);
    if (!relaysMe || !(relaysMe->loss & REPORT_RELAY_BIT)) continue;
    const Peer::ReportEntry *hop = findEntry(listener, relay.mac);
    if (!hop || entryLoss(hop) > RELAY_NEED_LOSS_PCT) continue;
    if (!found || entryLoss(hop) < loss) {
      loss = entryLoss(hop);
      rssi = hop->rssi;
      found = true;
    }
  }
  return found;
}

const char *volatile alertName = nullptr;

// warns about problems the jumper can act on, sparingly: outdated firmware and a full group
void updateAlerts(uint32_t now) {
  static uint32_t lastUpdateAlertMs = 0, lastFullAlertMs = 0, lastFullCount = 0;
  if (!settings.alerts || msSince(bootMs, now) < ALERT_GRACE_MS || pendingAlert) return;

  bool selfOutdated = newerFirmwareSeenMs && msSince(newerFirmwareSeenMs, now) < VERSION_SEEN_MS;
  bool otherOutdated = olderFirmwareSeenMs && msSince(olderFirmwareSeenMs, now) < VERSION_SEEN_MS;
  if ((selfOutdated || otherOutdated) && (!lastUpdateAlertMs || msSince(lastUpdateAlertMs, now) > ALERT_REPEAT_MS)) {
    pendingAlert = UPDATE_CHIME;
    lastUpdateAlertMs = now;
    alertName = selfOutdated ? "this unit needs a firmware update" : "a nearby unit needs a firmware update";
    return;
  }
  if (rxPeerTableFull != lastFullCount) {
    lastFullCount = rxPeerTableFull;
    if (!lastFullAlertMs || msSince(lastFullAlertMs, now) > ALERT_REPEAT_MS) {
      pendingAlert = GROUP_FULL_CHIME;
      lastFullAlertMs = now;
      alertName = "group full";
    }
  }
}

// picks the profile for the worst listener: degrade at once, upgrade only after the link stayed better
void adaptProfile(bool force) {
  static uint32_t lastRunMs = 0;
  static uint32_t betterSinceMs = 0;
  static uint32_t collapseSinceMs = 0;
  uint32_t now = millis();
  if (!force && now - lastRunMs < ADAPT_INTERVAL_MS) return;
  lastRunMs = now;

  int listeners = 0, senders = 1, worstRssi = 0, served = 0;
  float worstLoss = 0;
  for (Peer &p : peers) {
    if (!p.used) continue;
    if (msSince(p.lastAudioMs, now) < PEER_AUDIO_ACTIVE_MS) senders++;
    if (msSince(p.aboutMeMs, now) > REPORT_FRESH_MS) continue;
    listeners++;
    float loss = p.aboutMeLossPct;
    int rssi = p.aboutMeRssi;
    if (loss > RELAY_NEED_LOSS_PCT && relayedQuality(p, now, loss, rssi)) served++;
    worstLoss = max(worstLoss, loss);
    worstRssi = min(worstRssi, rssi);
  }
  relayServedListeners = served;
  updateAlerts(now);
  relayingCount = updateRelays();
  senders += relayingCount;
  adaptListeners = listeners;
  adaptSenders = senders;
  worstListenerLossPct = worstLoss;
  worstListenerRssi = worstRssi;

  Profile next;
  if (!settings.adaptive) {
    next = {settings.lc3Kbps, settings.redundantKbps, settings.redundancy};
  } else {
    uint8_t wanted = linkLevel;
    if (listeners > 0) {
      bool goodSignal = worstRssi > (linkLevel == LINK_GOOD ? ADAPT_GOOD_EXIT_RSSI : ADAPT_GOOD_ENTER_RSSI);
      bool poorSignal = worstRssi < (linkLevel == LINK_POOR ? ADAPT_POOR_EXIT_RSSI : ADAPT_POOR_ENTER_RSSI);
      bool collapsing = worstLoss > ADAPT_COLLAPSE_LOSS_PCT;
      if (!collapsing) collapseSinceMs = 0;
      else if (!collapseSinceMs) collapseSinceMs = now;
      bool collapsed = collapsing && now - collapseSinceMs >= ADAPT_COLLAPSE_HOLD_MS;
      wanted = poorSignal || collapsed ? LINK_POOR : goodSignal ? LINK_GOOD : LINK_MEDIUM;
      if (collapsing && !collapsed && wanted < linkLevel) wanted = linkLevel;
    }
    if (wanted > linkLevel) {
      linkLevel = wanted;
      betterSinceMs = 0;
    } else if (wanted < linkLevel) {
      if (!betterSinceMs) betterSinceMs = now;
      if (now - betterSinceMs > ADAPT_UPGRADE_HOLD_MS) {
        linkLevel = wanted;
        betterSinceMs = 0;
      }
    } else {
      betterSinceMs = 0;
    }
    next = fitAirtime(LEVEL_PROFILES[linkLevel], senders);
  }
  if (next != live) {
    live = next;
    profileChanges++;
  }
}

void encodeAndQueue(const int16_t *block) {
  static uint8_t main[LC3_MAX_FRAME_BYTES];
  static uint8_t redundant[LC3_MAX_FRAME_BYTES];
  int mainBytes = frameBytesFor(live.kbps);
  int redundantBytes = frameBytesFor(live.redundantKbps);
  uint32_t t0 = micros();
  if (lc3_encode(mainEncoder, LC3_PCM_FORMAT_S16, block, 1, mainBytes, main) < 0) encodeErrors++;
  encodeUsTotal += micros() - t0;
  encodeCount++;
  if (live.depth > 0 &&
      lc3_encode(redundantEncoder, LC3_PCM_FORMAT_S16, block, 1, redundantBytes, redundant) < 0) {
    encodeErrors++;
  }
  packetSink.addFrame(main, mainBytes, redundant, redundantBytes);
}

void produceFrame(int16_t *block) {
  if (encoderReconfigurePending) {
    encoderReconfigurePending = false;
    packetSink.reset();
    adaptProfile(true);
  }
  if (settings.hostSource) {
    readHostFrame(block);
  } else {
    for (int i = 0; i < SAMPLES_PER_FRAME; i++) block[i] = sineWave.readSample();
  }
  // a muted unit keeps announcing itself and reporting what it hears, so senders still adapt to it
  if (!settings.txEnabled) {
    static int idleFrames = 0;
    if (++idleFrames >= FRAMES_PER_PACKET * REPORT_EVERY_PACKETS) {
      idleFrames = 0;
      packetSink.sendKeepalive();
    }
    return;
  }
  txScope.capture(block, SAMPLES_PER_FRAME);
  encodeAndQueue(block);
}

void IRAM_ATTR onTxTimer(void *) { xTaskNotifyGive(txTaskHandle); }

void onSlotShift(void *) {
  esp_timer_start_periodic(txTimer, FRAME_US);
  xTaskNotifyGive(txTaskHandle);
}

int collectPeerPhases(int32_t *phases) {
  uint32_t now = millis();
  int n = 0;
  for (Peer &p : peers) {
    if (p.used && p.phaseValid && msSince(p.lastSeenMs, now) < PEER_ACTIVE_MS) phases[n++] = p.phaseUs;
  }
  return n;
}

int32_t widestGapMidpoint(int32_t *phases, int n) {
  std::sort(phases, phases + n);
  int32_t bestStart = phases[0], bestGap = 0;
  for (int i = 0; i < n; i++) {
    int32_t next = i + 1 < n ? phases[i + 1] : phases[0] + PACKET_US;
    if (next - phases[i] > bestGap) {
      bestGap = next - phases[i];
      bestStart = phases[i];
    }
  }
  return (bestStart + bestGap / 2) % PACKET_US;
}

// self-organising time slots: a sender that finds another one too close in the packet cycle moves
// to the middle of the widest free gap; the coin flip keeps two crowded senders from moving together
void maybeMoveSlot() {
  static uint32_t nextCheckMs = 0;
  if (millis() < nextCheckMs || ownPhaseUs < 0) return;
  nextCheckMs = millis() + SLOT_CHECK_MS + esp_random() % SLOT_CHECK_MS;

  int32_t phases[MAX_PEERS];
  int n = collectPeerPhases(phases);
  // a relay holds the air for its own packet plus the copies it forwards right after it
  int32_t packetAir = airtimeUs(packetSink.lastPacketSize);
  int32_t ownWindow = packetAir * (1 + relayingCount) + SLOT_MARGIN_US;
  int32_t before = packetAir + SLOT_MARGIN_US;
  bool conflict = false;
  for (int i = 0; i < n; i++) {
    int32_t ahead = ((phases[i] - ownPhaseUs) % (int32_t)PACKET_US + PACKET_US) % PACKET_US;
    conflict |= ahead < ownWindow || PACKET_US - ahead < before;
  }
  if (!conflict || (esp_random() & 1)) return;

  // the packet completes on the second frame after the restart, so the first frame goes one frame early
  int32_t target = widestGapMidpoint(phases, n);
  int64_t now = esp_timer_get_time();
  int64_t wait = ((target - (int32_t)FRAME_US - packetPhase(now)) % (int32_t)PACKET_US + PACKET_US) % PACKET_US;
  if (wait < 1000) wait += PACKET_US;
  esp_timer_stop(txTimer);
  esp_timer_start_once(slotShiftTimer, wait);
  slotMoves++;
}

// paced by a hardware timer instead of loop() so serial traffic cannot bunch packets together;
// a late wake-up produces every frame it owes so the sample clock never slips
void txTask(void *) {
  static int16_t block[SAMPLES_PER_FRAME];
  esp_task_wdt_add(nullptr);
  while (true) {
    uint32_t owed = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    esp_task_wdt_reset();
    if (owed > 1) txCatchUpFrames += owed - 1;
    adaptProfile(false);
    for (uint32_t i = 0; i < owed; i++) produceFrame(block);
    if (packetJustSent) {
      packetJustSent = false;
      if (settings.txEnabled) maybeMoveSlot();
    }
  }
}

void setupSources() {
  hostStream = xStreamBufferCreate(HOST_RING_SAMPLES * sizeof(int16_t), 1);
  sineWave.begin(info, activeTone());
  testWave.begin(info, activeTone());
  mainEncoder = lc3_setup_encoder(LC3_FRAME_US, SAMPLE_RATE, 0, &mainEncoderMemory);
  redundantEncoder = lc3_setup_encoder(LC3_FRAME_US, SAMPLE_RATE, 0, &redundantEncoderMemory);

  relayQueue = xQueueCreate(RELAY_QUEUE_DEPTH, sizeof(RelayItem));
  xTaskCreatePinnedToCore(relayTask, "relay", 4096, nullptr, 6, &relayTaskHandle, 0);
  xTaskCreatePinnedToCore(txTask, "tx", 8192, nullptr, 5, &txTaskHandle, 0);
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = onTxTimer;
  timerArgs.name = "tx";
  esp_timer_create(&timerArgs, &txTimer);
  esp_timer_create_args_t shiftArgs = {};
  shiftArgs.callback = onSlotShift;
  shiftArgs.name = "slot";
  esp_timer_create(&shiftArgs, &slotShiftTimer);
  esp_timer_start_periodic(txTimer, FRAME_US);
}
