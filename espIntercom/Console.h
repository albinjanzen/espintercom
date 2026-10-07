#pragma once

#include "Source.h"

uint32_t loopMaxGapUs = 0;
TaskHandle_t mixerTaskHandle;
volatile bool cpuStatsEnabled = true;
volatile uint32_t statsIntervalMs = STATS_INTERVAL_MS;
volatile uint32_t serialRxBytes = 0;
volatile uint32_t serialJunkBytes = 0;
volatile uint32_t uartOverflows = 0;
volatile uint32_t uartFrameErrors = 0;
volatile unsigned rxAvailMax = 0;

volatile uint32_t uartBufferFull = 0;

void onSerialError(hardwareSerial_error_t error) {
  if (error == UART_FIFO_OVF_ERROR) uartOverflows++;
  else if (error == UART_BUFFER_FULL_ERROR) uartBufferFull++;
  else uartFrameErrors++;
}

struct CpuLoad {
  float core[2] = {0, 0};
  float tx = 0, mixer = 0, relay = 0, wifi = 0;
};

// share of the last stats interval each core spent outside its idle task, from FreeRTOS run-time counters
CpuLoad measureCpuLoad() {
  static TaskStatus_t tasks[40];
  static uint32_t lastTotal = 0, lastIdle[2] = {0, 0}, lastTx = 0, lastMixer = 0, lastRelay = 0, lastWifi = 0;
  uint32_t total = 0;
  UBaseType_t count = uxTaskGetSystemState(tasks, 40, &total);
  uint32_t idle[2] = {0, 0}, tx = 0, mixer = 0, relay = 0, wifi = 0;
  for (UBaseType_t i = 0; i < count; i++) {
    const char *name = tasks[i].pcTaskName;
    uint32_t t = tasks[i].ulRunTimeCounter;
    if (!strcmp(name, "IDLE0")) idle[0] = t;
    else if (!strcmp(name, "IDLE1")) idle[1] = t;
    else if (!strcmp(name, "tx")) tx = t;
    else if (!strcmp(name, "mixer")) mixer = t;
    else if (!strcmp(name, "relay")) relay = t;
    else if (!strcmp(name, "wifi")) wifi = t;
  }
  CpuLoad load;
  uint32_t span = total - lastTotal;
  if (lastTotal && span) {
    for (int c = 0; c < 2; c++) load.core[c] = 100.0f - 100.0f * (idle[c] - lastIdle[c]) / span;
    load.tx = 100.0f * (tx - lastTx) / span;
    load.mixer = 100.0f * (mixer - lastMixer) / span;
    load.relay = 100.0f * (relay - lastRelay) / span;
    load.wifi = 100.0f * (wifi - lastWifi) / span;
  }
  lastTotal = total;
  lastIdle[0] = idle[0];
  lastIdle[1] = idle[1];
  lastTx = tx;
  lastMixer = mixer;
  lastRelay = relay;
  lastWifi = wifi;
  return load;
}

const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void printMac(const uint8_t *mac) {
  Serial.printf("\"%02X:%02X:%02X:%02X:%02X:%02X\"", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void reply(const char *fmt, ...) {
  char msg[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("{\"type\":\"reply\",\"msg\":\"%s\"}\n", msg);
}

const char *blockedName() {
  static char name[6];
  if (!blockActive) return "";
  snprintf(name, sizeof(name), "%02X:%02X", blockedTail[0], blockedTail[1]);
  return name;
}

void printBoot() {
  Serial.print("{\"type\":\"boot\",\"node\":");
  printMac(ownMac);
  Serial.printf(
      ",\"reset\":\"%s\",\"session\":%u,\"tone\":%d,\"channel\":%d,\"rate\":%.1f,"
      "\"pins\":{\"bck\":%d,\"ws\":%d,\"data\":%d}}\n",
      resetReasonName(), ownSession, activeTone(), WIFI_CHANNEL, settings.rateTenthMbps / 10.0f,
      settings.pinBck, settings.pinWs, settings.pinData);
}

void printPeers(uint32_t now) {
  bool first = true;
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer &p = peers[i];
    PeerAudio &a = peerAudio[i];
    if (!p.used) continue;
    if (!first) Serial.print(",");
    first = false;
    Serial.print("{\"mac\":");
    printMac(p.mac);
    Serial.printf(
        ",\"active\":%s,\"playing\":%s,\"rx\":%lu,\"lost\":%lu,\"recovered\":%lu,\"late\":%lu,"
        "\"restarts\":%lu,\"rssi\":%d,\"bufferMs\":%.1f,\"underruns\":%lu,\"plc\":%lu,\"decodeErrors\":%lu,\"concealed\":%lu,"
        "\"debtSkipped\":%lu,\"driftDrops\":%lu,\"driftInserts\":%lu,\"overflows\":%lu,\"silentMs\":%lu,\"maxLateMs\":%.1f,\"targetMs\":%.1f,\"slotGapUs\":%ld,"
        "\"aboutMeLossPct\":%.1f,\"aboutMeRssi\":%d,\"aboutMeAgeMs\":%lu,"
        "\"directLossPct\":%.1f,\"viaRelay\":%lu,\"relayingFor\":%s}",
        msSince(p.lastSeenMs, now) < PEER_ACTIVE_MS ? "true" : "false", a.mixing ? "true" : "false",
        p.received, p.lost, p.recovered, p.late, p.restarts, p.rssi,
        a.bufferedSamples * 1000.0f / info.sample_rate, a.underruns, a.decoderConcealed, a.decodeErrors, a.concealedFrames,
        a.debtSkipped, a.driftDrops, a.driftInserts, a.ring.overflows, msSince(p.lastSeenMs, now), p.maxLateUs / 1000.0f,
        a.target * 1000.0f / info.sample_rate,
        (long)(p.phaseValid && ownPhaseUs >= 0 ? phaseDistance(p.phaseUs, ownPhaseUs) : -1),
        (float)p.aboutMeLossPct, (int)p.aboutMeRssi, p.aboutMeMs ? msSince(p.aboutMeMs, now) : 0UL,
        p.directLossEma, p.relayedReceived, p.relaying ? "true" : "false");
    p.maxLateUs = 0;
  }
}

void printStats() {
  static uint32_t lastPrint = 0;
  uint32_t now = millis();
  if (now - lastPrint < statsIntervalMs) return;
  lastPrint = now;

  CpuLoad cpu = cpuStatsEnabled ? measureCpuLoad() : CpuLoad();
  Serial.print("{\"type\":\"stats\",\"node\":");
  printMac(ownMac);
  Serial.printf(",\"cpu0\":%.1f,\"cpu1\":%.1f,\"cpuTx\":%.1f,\"cpuMixer\":%.1f,\"cpuRelay\":%.1f,\"cpuWifi\":%.1f",
                cpu.core[0], cpu.core[1], cpu.tx, cpu.mixer, cpu.relay, cpu.wifi);
  Serial.printf(
      ",\"uptimeMs\":%lu,\"reset\":\"%s\",\"session\":%u,\"heap\":%lu,\"tx\":%lu,\"txFail\":%lu,"
      "\"qDrop\":%lu,\"qFill\":%u,\"qSize\":%d,\"loopMaxGapUs\":%lu,\"mixerFrames\":%lu,"
      "\"periodMinUs\":%lu,\"periodMaxUs\":%lu,\"mixed\":%u,\"clipped\":%lu,\"mixerUs\":%lu,"
      "\"invalid\":%lu,\"wrongVersion\":%lu,\"peerTableFull\":%lu,\"txEnabled\":%s,"
      "\"playEnabled\":%s,\"testTone\":%s,\"scope\":%s,\"tone\":%d,\"volume\":%d,\"rate\":%.1f,"
      "\"packetBytes\":%u,\"airtimeUs\":%u,\"source\":\"%s\",\"lc3Kbps\":%d,\"redundantKbps\":%d,"
      "\"redundancy\":%d,\"adaptive\":%s,\"link\":\"%s\",\"liveKbps\":%d,\"liveRedundantKbps\":%d,"
      "\"liveDepth\":%d,\"listeners\":%u,\"senders\":%u,\"worstLossPct\":%.1f,\"worstRssi\":%d,"
      "\"alerts\":%s,\"groupVersion\":%lu,\"groupOwner\":\"%02X:%02X\",\"configAdoptions\":%lu,\"profileChanges\":%lu,\"relaying\":%u,\"relayServed\":%u,\"relaySent\":%lu,\"relayDrops\":%lu,\"blocked\":\"%s\",\"simLossPct\":%.1f,\"encodeErrors\":%lu,\"encodeUs\":%lu,\"decodeUs\":%lu,\"decodeUsMax\":%lu,\"eq\":%s,"
      "\"espnowVersion\":%lu,\"hostBufMs\":%.1f,\"hostUnderruns\":%lu,\"hostDrift\":%lu,"
      "\"hostInserts\":%lu,\"hostBytes\":%lu,\"serialRx\":%lu,\"serialJunk\":%lu,\"uartOverflows\":%lu,\"uartBufferFull\":%lu,\"rxAvailMax\":%u,\"uartFrameErrors\":%lu,\"hostOverflow\":%lu,\"txCatchUp\":%lu,"
      "\"slotPhaseUs\":%ld,\"slotMoves\":%lu,\"glitches\":%lu,\"stackMixer\":%u,\"stackTx\":%u,\"stackLoop\":%u,\"pins\":{\"bck\":%d,\"ws\":%d,\"data\":%d},\"peers\":[",
      now, resetReasonName(), ownSession, (unsigned long)ESP.getFreeHeap(), txPackets, txFailures,
      rxQueueDrops, (unsigned)uxQueueMessagesWaiting(rxQueue), RX_QUEUE_DEPTH, loopMaxGapUs,
      mixerFrames, mixerPeriodMinUs, mixerPeriodMaxUs, (unsigned)mixedPeers, clippedSamples,
      mixerBusyUs, rxInvalid, rxWrongVersion, rxPeerTableFull,
      settings.txEnabled ? "true" : "false", settings.playEnabled ? "true" : "false",
      localTestTone ? "true" : "false", scopeEnabled ? "true" : "false", activeTone(),
      settings.volume, settings.rateTenthMbps / 10.0f, (unsigned)packetSink.lastPacketSize,
      (unsigned)airtimeUs(packetSink.lastPacketSize), settings.hostSource ? "host" : "sine",
      settings.lc3Kbps, settings.redundantKbps, settings.redundancy, settings.adaptive ? "true" : "false",
      settings.adaptive ? LINK_LEVEL_NAMES[linkLevel] : "manual", live.kbps, live.redundantKbps, live.depth,
      (unsigned)adaptListeners, (unsigned)adaptSenders, (float)worstListenerLossPct, (int)worstListenerRssi,
      settings.alerts ? "true" : "false", settings.groupVersion, settings.groupOwner[4], settings.groupOwner[5], configAdoptions,
      profileChanges, (unsigned)relayingCount, (unsigned)relayServedListeners, relaySent, relayDrops, blockedName(), simLossPermille / 10.0f,
      encodeErrors,
      encodeCount ? encodeUsTotal / encodeCount : 0, decodeCount ? decodeUsTotal / decodeCount : 0, decodeUsMax,
      settings.voiceEq ? "true" : "false",
      espnowVersion, hostSamplesAvailable() * 1000.0f / info.sample_rate, hostUnderruns,
      hostDriftDrops, hostDriftInserts, hostBytes, serialRxBytes, serialJunkBytes, uartOverflows, uartBufferFull, rxAvailMax, uartFrameErrors, hostOverflowBytes, txCatchUpFrames,
      (long)ownPhaseUs, slotMoves, glitchCount, uxTaskGetStackHighWaterMark(mixerTaskHandle), uxTaskGetStackHighWaterMark(txTaskHandle),
      uxTaskGetStackHighWaterMark(nullptr), settings.pinBck, settings.pinWs, settings.pinData);
  printPeers(now);
  Serial.print("],\"eqBands\":[");
  for (int i = 0; i < EQ_BANDS; i++) {
    const EqBand &b = settings.eq[i];
    Serial.printf("%s[%d,%d,%.1f]", i ? "," : "", b.hz, b.db, b.q10 / 10.0f);
  }
  Serial.println("]}");
  mixerPeriodMaxUs = 0;
  mixerPeriodMinUs = UINT32_MAX;
  loopMaxGapUs = 0;
  encodeUsTotal = encodeCount = decodeUsTotal = decodeCount = decodeUsMax = 0;
}

void printAlerts() {
  const char *name = alertName;
  if (!name) return;
  alertName = nullptr;
  reply("alert: %s", name);
}

void printGlitches() {
  static const char *names[] = {"conceal", "debt-skip", "prime", "stretch", "underrun", "hard-skip",
                                "chime", "fade-in", "late-mixer", "decoder-reset", "burst"};
  GlitchEvent ev;
  while (xQueueReceive(glitchQueue, &ev, 0) == pdTRUE) {
    Serial.printf("{\"type\":\"glitch\",\"ms\":%lu,\"jump\":%d,\"periodUs\":%lu,\"mixed\":%u,\"events\":[",
                  ev.ms, ev.jump, ev.periodUs, ev.mixed);
    bool first = true;
    for (int i = 0; i < 11; i++) {
      if (!(ev.events & (1 << i))) continue;
      Serial.printf("%s\"%s\"", first ? "" : ",", names[i]);
      first = false;
    }
    Serial.println("]}");
  }
}

// a local change becomes the newest group setting and spreads to every unit that hears this one
void claimGroupSettings() {
  settings.groupVersion++;
  memcpy(settings.groupOwner, ownMac, 6);
  markSettingsDirty();
}

void applyPendingConfig() {
  if (!configPending) return;
  GroupConfig c = pendingConfig;
  configPending = false;
  if (!newerConfig(c.version, c.owner)) return;
  bool rateChanged = c.rateTenthMbps != settings.rateTenthMbps;
  settings.groupVersion = c.version;
  memcpy(settings.groupOwner, c.owner, 6);
  settings.rateTenthMbps = c.rateTenthMbps;
  settings.adaptive = c.adaptive;
  settings.lc3Kbps = c.lc3Kbps;
  settings.redundantKbps = c.redundantKbps;
  settings.redundancy = c.redundancy;
  sanitizeSettings();
  if (rateChanged) applyRate();
  encoderReconfigurePending = true;
  markSettingsDirty();
  configAdoptions++;
  reply("adopted group settings v%lu from %02X:%02X", c.version, c.owner[4], c.owner[5]);
}

void restartNow(const char *why) {
  flushSettings();
  reply("%s, rebooting", why);
  Serial.flush();
  ESP.restart();
}

void resetStats() {
  txPackets = txFailures = rxQueueDrops = rxInvalid = rxWrongVersion = rxPeerTableFull = 0;
  clippedSamples = hostUnderruns = hostDriftDrops = hostDriftInserts = hostBytes = 0;
  hostOverflowBytes = txCatchUpFrames = glitchCount = slotMoves = encodeErrors = profileChanges = 0;
  relaySent = relayDrops = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    PeerAudio &a = peerAudio[i];
    a.decoderConcealed = a.decodeErrors = 0;
    a.underruns = a.concealedFrames = a.debtSkipped = a.driftDrops = a.driftInserts = a.ring.overflows = 0;
  }
  resetStatsRequested = true;
}

void handleCommand(String line) {
  line.trim();
  if (line.isEmpty()) return;
  int space = line.indexOf(' ');
  String cmd = space < 0 ? line : line.substring(0, space);
  String arg = space < 0 ? "" : line.substring(space + 1);
  arg.trim();

  if (cmd == "tx") {
    settings.txEnabled = arg == "on";
    markSettingsDirty();
    reply("tx %s", settings.txEnabled ? "on" : "off");
  } else if (cmd == "play") {
    settings.playEnabled = arg == "on";
    markSettingsDirty();
    reply("play %s", settings.playEnabled ? "on" : "off");
  } else if (cmd == "test") {
    localTestTone = arg == "on";
    reply("local test tone %s", localTestTone ? "on" : "off");
  } else if (cmd == "src") {
    selectHostSource(arg == "host");
    reply("source %s", settings.hostSource ? "host" : "sine");
  } else if (cmd == "kbps" || cmd == "rkbps") {
    int value = arg.toInt();
    int upper = cmd == "kbps" ? MAX_LC3_KBPS : settings.lc3Kbps;
    if (value < MIN_LC3_KBPS || value > upper) {
      reply("%s must be %d..%d", cmd.c_str(), MIN_LC3_KBPS, upper);
      return;
    }
    if (cmd == "kbps") {
      settings.lc3Kbps = value;
      settings.redundantKbps = min(settings.redundantKbps, value);
    } else {
      settings.redundantKbps = value;
    }
    claimGroupSettings();
    encoderReconfigurePending = true;
    reply("lc3 %d kbps, redundant %d kbps", settings.lc3Kbps, settings.redundantKbps);
  } else if (cmd == "redundancy") {
    int value = arg.toInt();
    if (arg.isEmpty() || value < 0 || value > MAX_REDUNDANCY) {
      reply("redundancy must be 0..%d", MAX_REDUNDANCY);
      return;
    }
    settings.redundancy = value;
    claimGroupSettings();
    encoderReconfigurePending = true;
    reply("redundancy %d", settings.redundancy);
  } else if (cmd == "eqband") {
    int i, hz, db;
    float q;
    if (sscanf(arg.c_str(), "%d %d %d %f", &i, &hz, &db, &q) != 4 || i < 0 || i >= EQ_BANDS ||
        hz < 50 || hz > 7800 || db < -15 || db > 15 || q < 0.3f || q > 10) {
      reply("usage: eqband <0..%d> <50..7800 Hz> <-15..15 dB> <q 0.3..10>", EQ_BANDS - 1);
      return;
    }
    settings.eq[i] = {(int16_t)hz, (int8_t)db, (uint8_t)roundf(q * 10)};
    markSettingsDirty();
    setupVoiceEq();
    reply("eq band %d: %d Hz %d dB q %.1f", i, hz, db, q);
  } else if (cmd == "eqreset") {
    memcpy(settings.eq, DEFAULT_EQ, sizeof(settings.eq));
    markSettingsDirty();
    setupVoiceEq();
    reply("eq reset to defaults");
  } else if (cmd == "eq") {
    settings.voiceEq = arg == "on";
    markSettingsDirty();
    reply("voice eq %s", settings.voiceEq ? "on" : "off");
  } else if (cmd == "adapt") {
    settings.adaptive = arg == "on";
    claimGroupSettings();
    encoderReconfigurePending = true;
    reply("adaptive %s", settings.adaptive ? "on" : "off");
  } else if (cmd == "chime") {
    int count = 0;
    char name[24] = {0};
    sscanf(arg.c_str(), "%23s %d", name, &count);
    String which(name);
    const ChimeNote *notes = nullptr;
    if (which == "startup") notes = STARTUP_CHIME;
    else if (which == "status") notes = composeStatus(max(count, 1));
    else if (which == "join") notes = JOIN_CHIME;
    else if (which == "leave") notes = LEAVE_CHIME;
    else if (which == "update") notes = UPDATE_CHIME;
    else if (which == "full") notes = GROUP_FULL_CHIME;
    if (!notes) {
      reply("usage: chime startup|status N|join|leave|update|full");
      return;
    }
    chime.request(notes);
    reply("playing %s %d", name, count);
  } else if (cmd == "protover") {
    ownProtocolVersion = arg.isEmpty() ? PROTOCOL_VERSION : constrain(arg.toInt(), 1, 255);
    reply("protocol version %u%s", ownProtocolVersion, ownProtocolVersion == PROTOCOL_VERSION ? "" : " (test)");
  } else if (cmd == "maxpeers") {
    peerLimit = constrain(arg.isEmpty() ? MAX_PEERS : arg.toInt(), 1, MAX_PEERS);
    for (int i = 0; i < MAX_PEERS; i++) peers[i].used = false;
    reply("peer limit %u, table cleared", peerLimit);
  } else if (cmd == "statsms") {
    statsIntervalMs = constrain(arg.toInt(), 200, 60000);
    reply("stats every %lu ms", statsIntervalMs);
  } else if (cmd == "cpustats") {
    cpuStatsEnabled = arg == "on";
    reply("cpu stats %s", cpuStatsEnabled ? "on" : "off");
  } else if (cmd == "alerts") {
    settings.alerts = arg == "on";
    markSettingsDirty();
    reply("alerts %s", settings.alerts ? "on" : "off");
  } else if (cmd == "simloss") {
    simLossPermille = constrain((int)roundf(arg.toFloat() * 10), 0, 1000);
    reply("simulated receive loss %.1f%%", simLossPermille / 10.0f);
  } else if (cmd == "block") {
    unsigned a, b;
    if (sscanf(arg.c_str(), "%x:%x", &a, &b) == 2) {
      blockedTail[0] = a;
      blockedTail[1] = b;
      blockActive = true;
      reply("ignoring direct packets from ..%s", blockedName());
    } else {
      blockActive = false;
      reply("block off");
    }
  } else if (cmd == "scope") {
    scopeEnabled = arg == "on";
    reply("scope %s", scopeEnabled ? "on" : "off");
  } else if (cmd == "tone") {
    settings.toneHz = arg.toInt();
    markSettingsDirty();
    sineWave.setFrequency(activeTone());
    testWave.setFrequency(activeTone());
    reply("tone %d Hz", activeTone());
  } else if (cmd == "vol") {
    settings.volume = constrain(arg.toInt(), 0, 100);
    markSettingsDirty();
    applyVolume();
    reply("volume %d", settings.volume);
  } else if (cmd == "rate") {
    int previous = settings.rateTenthMbps;
    settings.rateTenthMbps = (int)roundf(arg.toFloat() * 10);
    if (!applyRate()) {
      settings.rateTenthMbps = previous;
      reply("invalid rate, use 1 2 5.5 11 6 9 12 18 24");
      return;
    }
    claimGroupSettings();
    reply("rate %.1f Mbps", settings.rateTenthMbps / 10.0f);
  } else if (cmd == "pins") {
    int bck, ws, data;
    if (sscanf(arg.c_str(), "%d %d %d", &bck, &ws, &data) != 3) {
      reply("usage: pins <bck> <ws> <data>");
      return;
    }
    settings.pinBck = bck;
    settings.pinWs = ws;
    settings.pinData = data;
    restartNow("pins saved");
  } else if (cmd == "reset") {
    resetStats();
    reply("stats reset");
  } else if (cmd == "reboot") {
    restartNow("reboot requested");
  } else {
    reply("unknown command: %s", cmd.c_str());
  }
}

// serial carries text command lines and binary audio frames: A5 5A <len u16 le> <s16le pcm>
void readSerial() {
  enum State { Text, Magic2, Len1, Len2, Payload };
  static State state = Text;
  static String line;
  static uint16_t frameLen = 0;
  static uint16_t frameRead = 0;
  static bool frameUlaw = false;
  static uint8_t frame[HOST_FRAME_MAX_BYTES];
  static uint8_t chunk[512];

  // bulk reads: per-byte Serial.read() takes a lock each call and stalls the send loop at audio rates
  size_t n;
  size_t avail = Serial.available();
  if (avail > rxAvailMax) rxAvailMax = avail;
  while ((n = Serial.read(chunk, min((size_t)Serial.available(), sizeof(chunk)))) > 0) {
    serialRxBytes += n;
    for (size_t i = 0; i < n; i++) {
      uint8_t c = chunk[i];
      switch (state) {
        case Text:
          if (c == HOST_FRAME_MAGIC_1) {
            state = Magic2;
          } else if (c == '\n' || c == '\r') {
            handleCommand(line);
            line = "";
          } else if (line.length() < 120) {
            if (c < 0x20 || c > 0x7e) serialJunkBytes++;
            line += (char)c;
          }
          break;
        case Magic2:
          frameUlaw = c == HOST_FRAME_MAGIC_ULAW;
          state = c == HOST_FRAME_MAGIC_2 || frameUlaw ? Len1 : Text;
          break;
        case Len1:
          frameLen = c;
          state = Len2;
          break;
        case Len2:
          frameLen |= c << 8;
          frameRead = 0;
          state = frameLen > 0 && frameLen <= HOST_FRAME_MAX_BYTES && (frameUlaw || frameLen % 2 == 0) ? Payload : Text;
          break;
        case Payload:
          frame[frameRead++] = c;
          if (frameRead == frameLen) {
            if (settings.hostSource) {
              if (frameUlaw) writeHostUlaw(frame, frameLen);
              else writeHostAudio(frame, frameLen);
            }
            state = Text;
          }
          break;
      }
    }
  }
}

void trackLoopGap() {
  static uint32_t lastLoop = micros();
  uint32_t nowUs = micros();
  if (nowUs - lastLoop > loopMaxGapUs) loopMaxGapUs = nowUs - lastLoop;
  lastLoop = nowUs;
}
