#pragma once

#include "Mixer.h"

// payload: `depth` earlier packets at the redundant bitrate (oldest first), then this packet's frames,
// then an optional reception report; a keepalive carries no frames, only the report
struct __attribute__((packed)) PacketHeader {
  uint16_t magic;
  uint8_t version;
  uint8_t frames;
  uint8_t depth;
  uint8_t flags;
  uint16_t frameBytes;
  uint16_t redundantBytes;
  uint16_t session;
  uint32_t seq;
};

uint8_t ownMac[6];
volatile int32_t ownPhaseUs = -1;
volatile bool packetJustSent = false;

int32_t packetPhase(int64_t timeUs) { return (int32_t)(timeUs % PACKET_US); }

int32_t phaseDistance(int32_t a, int32_t b) {
  int32_t d = abs(a - b);
  return min(d, (int32_t)PACKET_US - d);
}
uint16_t ownSession = 0;
uint32_t espnowVersion = 0;
volatile bool resetStatsRequested = false;
volatile uint32_t txPackets = 0;
volatile uint32_t txFailures = 0;
volatile uint32_t rxInvalid = 0;
volatile uint32_t rxWrongVersion = 0;
volatile uint32_t rxPeerTableFull = 0;
volatile uint32_t simLossPermille = 0;
volatile uint32_t newerFirmwareSeenMs = 0;
// test knobs: pretend to run another protocol version, or have room for fewer peers
volatile uint8_t ownProtocolVersion = PROTOCOL_VERSION;
volatile uint8_t peerLimit = MAX_PEERS;
volatile uint32_t olderFirmwareSeenMs = 0;

struct Profile {
  int kbps;
  int redundantKbps;
  int depth;
  bool operator!=(const Profile &o) const { return kbps != o.kbps || redundantKbps != o.redundantKbps || depth != o.depth; }
};

Profile live = {DEFAULT_LC3_KBPS, DEFAULT_REDUNDANT_KBPS, DEFAULT_REDUNDANCY};

struct __attribute__((packed)) GroupConfig {
  uint32_t version;
  uint8_t owner[6];
  uint8_t rateTenthMbps;
  uint8_t adaptive;
  uint8_t lc3Kbps;
  uint8_t redundantKbps;
  uint8_t redundancy;
};

GroupConfig pendingConfig;
volatile bool configPending = false;
volatile uint32_t configAdoptions = 0;

bool newerConfig(uint32_t version, const uint8_t *owner) {
  if (version != settings.groupVersion) return version > settings.groupVersion;
  return memcmp(owner, settings.groupOwner, 6) > 0;
}

size_t buildConfig(uint8_t *out) {
  GroupConfig c{settings.groupVersion, {0}, (uint8_t)settings.rateTenthMbps, settings.adaptive,
                (uint8_t)settings.lc3Kbps, (uint8_t)settings.redundantKbps, (uint8_t)settings.redundancy};
  memcpy(c.owner, settings.groupOwner, 6);
  memcpy(out, &c, sizeof(c));
  return sizeof(c);
}

// adopted on the loop task, which owns settings changes and the rate API
void receiveConfig(const uint8_t *data) {
  GroupConfig c;
  memcpy(&c, data, sizeof(c));
  if (configPending || !newerConfig(c.version, c.owner)) return;
  pendingConfig = c;
  configPending = true;
}

struct RelayItem {
  uint8_t origin[6];
  uint16_t len;
  uint8_t data[RELAY_MAX_PACKET_BYTES];
};

QueueHandle_t relayQueue;
TaskHandle_t relayTaskHandle;
volatile uint32_t relaySent = 0;
volatile uint32_t relayDrops = 0;
uint8_t blockedTail[2] = {0, 0};
volatile bool blockActive = false;

float directLossPct(Peer &p) {
  if (p.lastSeq < p.reportSeq || p.directReceived < p.reportDirect) {
    p.reportSeq = p.lastSeq;
    p.reportDirect = p.directReceived;
  }
  uint32_t expected = p.lastSeq - p.reportSeq;
  uint32_t direct = p.directReceived - p.reportDirect;
  float pct = expected ? 100.0f * (1.0f - (float)min(direct, expected) / expected) : p.directLossEma;
  p.reportSeq = p.lastSeq;
  p.reportDirect = p.directReceived;
  return pct;
}

// tells every sender how well this unit hears it directly, and which senders it relays for others
size_t buildReport(uint8_t *out) {
  uint32_t now = millis();
  uint8_t count = 0;
  uint8_t *entry = out + 1;
  for (Peer &p : peers) {
    if (!p.used || msSince(p.lastSeenMs, now) > PEER_ACTIVE_MS) continue;
    p.directLossEma += (directLossPct(p) - p.directLossEma) * REPORT_LOSS_SMOOTHING;
    memcpy(entry, p.mac + 3, 3);
    entry[3] = (uint8_t)roundf(min(p.directLossEma, REPORT_MAX_LOSS_PCT) * 2) | (p.relaying ? REPORT_RELAY_BIT : 0);
    entry[4] = (uint8_t)(int8_t)roundf(p.rssiEma);
    entry += REPORT_ENTRY_BYTES;
    count++;
  }
  out[0] = count;
  return 1 + count * REPORT_ENTRY_BYTES;
}

void parseReport(Peer &reporter, const uint8_t *entries, int count, uint32_t now) {
  count = min(count, MAX_PEERS);
  memcpy(reporter.entries, entries, count * REPORT_ENTRY_BYTES);
  reporter.entryCount = count;
  reporter.reportMs = now;
  for (int i = 0; i < count; i++) {
    if (memcmp(reporter.entries[i].mac3, ownMac + 3, 3) != 0) continue;
    reporter.aboutMeLossPct = (reporter.entries[i].loss & ~REPORT_RELAY_BIT) / 2.0f;
    reporter.aboutMeRssi = reporter.entries[i].rssi;
    reporter.aboutMeMs = now;
  }
}

const Peer::ReportEntry *findEntry(const Peer &reporter, const uint8_t *mac) {
  for (int i = 0; i < reporter.entryCount; i++) {
    if (memcmp(reporter.entries[i].mac3, mac + 3, 3) == 0) return &reporter.entries[i];
  }
  return nullptr;
}

float entryLoss(const Peer::ReportEntry *e) { return (e->loss & ~REPORT_RELAY_BIT) / 2.0f; }

class PacketSink {
 public:
  void addFrame(const uint8_t *main, int mainBytes, const uint8_t *redundant, int redundantFrameBytes) {
    if (frames == 0) {
      if (redundantFrameBytes != redundantBytes) historyCount = 0;
      frameBytes = mainBytes;
      redundantBytes = redundantFrameBytes;
    }
    memcpy(current + frames * frameBytes, main, frameBytes);
    memcpy(currentRedundant + frames * redundantBytes, redundant, redundantBytes);
    if (++frames == FRAMES_PER_PACKET) send();
  }

  // redundant copies at an old bitrate cannot share a packet with new ones, so a codec change drops them
  void reset() {
    frames = 0;
    historyCount = 0;
  }

  void sendRelayed(const uint8_t *original, size_t audioLen, const uint8_t *originMac) {
    PacketHeader header;
    memcpy(&header, original, sizeof(header));
    header.flags = (header.flags & ~(PACKET_FLAG_REPORT | PACKET_FLAG_CONFIG)) | PACKET_FLAG_RELAYED;
    memcpy(relayPacket, &header, sizeof(header));
    memcpy(relayPacket + sizeof(header), originMac, 6);
    memcpy(relayPacket + sizeof(header) + 6, original + sizeof(header), audioLen - sizeof(header));
    if (esp_now_send(BROADCAST_ADDR, relayPacket, audioLen + 6) == ESP_OK) relaySent++;
    else txFailures++;
  }

  void sendKeepalive() {
    PacketHeader header{PACKET_MAGIC, ownProtocolVersion, 0, 0, PACKET_FLAG_REPORT | PACKET_FLAG_CONFIG, 0, 0,
                        ownSession, seq++};
    memcpy(packet, &header, sizeof(header));
    size_t len = sizeof(header) + buildReport(packet + sizeof(header));
    len += buildConfig(packet + len);
    if (esp_now_send(BROADCAST_ADDR, packet, len) == ESP_OK) txPackets++;
    else txFailures++;
  }

  size_t lastPacketSize = 0;

 private:
  uint8_t packet[MAX_PACKET_BYTES];
  uint8_t relayPacket[RELAY_MAX_PACKET_BYTES + 6];
  uint8_t current[MAX_SEGMENT_BYTES];
  uint8_t currentRedundant[MAX_SEGMENT_BYTES];
  uint8_t history[MAX_REDUNDANCY][MAX_SEGMENT_BYTES];
  int historyCount = 0;
  int frames = 0;
  int frameBytes = 0;
  int redundantBytes = 0;
  uint32_t seq = 0;

  void send() {
    int depth = min(live.depth, historyCount);
    size_t segment = FRAMES_PER_PACKET * redundantBytes;
    size_t offset = sizeof(PacketHeader);
    for (int i = historyCount - depth; i < historyCount; i++) {
      memcpy(packet + offset, history[i], segment);
      offset += segment;
    }
    memcpy(packet + offset, current, FRAMES_PER_PACKET * frameBytes);
    offset += FRAMES_PER_PACKET * frameBytes;
    bool withReport = seq % REPORT_EVERY_PACKETS == 0;
    bool withConfig = seq % CONFIG_EVERY_PACKETS == 0;
    if (withReport) offset += buildReport(packet + offset);
    if (withConfig) offset += buildConfig(packet + offset);
    lastPacketSize = offset;
    uint8_t flags = (withReport ? PACKET_FLAG_REPORT : 0) | (withConfig ? PACKET_FLAG_CONFIG : 0);
    PacketHeader header{PACKET_MAGIC, ownProtocolVersion, (uint8_t)frames, (uint8_t)depth, flags, (uint16_t)frameBytes,
                        (uint16_t)redundantBytes, ownSession, seq++};
    memcpy(packet, &header, sizeof(header));

    delayMicroseconds(esp_random() % TX_JITTER_US);
    if (esp_now_send(BROADCAST_ADDR, packet, lastPacketSize) == ESP_OK) {
      txPackets++;
    } else {
      txFailures++;
    }
    ownPhaseUs = packetPhase(esp_timer_get_time());
    packetJustSent = true;
    if (relayTaskHandle) xTaskNotifyGive(relayTaskHandle);

    if (historyCount == MAX_REDUNDANCY) {
      memmove(history[0], history[1], sizeof(history[0]) * (MAX_REDUNDANCY - 1));
      historyCount--;
    }
    memcpy(history[historyCount++], currentRedundant, segment);
    frames = 0;
  }
};

PacketSink packetSink;

const RateOption *findRate(int tenthMbps) {
  for (const RateOption &r : RATES) {
    if (r.tenthMbps == tenthMbps) return &r;
  }
  return nullptr;
}

bool applyRate() {
  const RateOption *r = findRate(settings.rateTenthMbps);
  if (r == nullptr) return false;
  esp_now_rate_config_t cfg = {};
  cfg.phymode = r->mode;
  cfg.rate = r->rate;
  return esp_now_set_peer_rate_config(BROADCAST_ADDR, &cfg) == ESP_OK;
}

int findOrAddPeer(const uint8_t *mac, uint32_t now) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && memcmp(peers[i].mac, mac, 6) == 0) return i;
  }
  // units still present are never displaced; a newcomer only gets the slot of one that has been gone longest
  int slot = -1;
  uint32_t longestGone = PEER_REUSE_MS;
  for (int i = 0; i < peerLimit; i++) {
    if (!peers[i].used) {
      slot = i;
      break;
    }
    uint32_t gone = msSince(peers[i].lastSeenMs, now);
    if (gone > longestGone) {
      longestGone = gone;
      slot = i;
    }
  }
  if (slot < 0) return -1;
  memset(&peers[slot], 0, sizeof(Peer));
  memcpy(peers[slot].mac, mac, 6);
  peers[slot].used = true;
  peerAudio[slot].generation++;
  peerAudio[slot].resetPending = true;
  return slot;
}

// returns how many packets were missed right before this one, or -1 for a late duplicate
int trackSequence(Peer &peer, uint32_t seq) {
  peer.received++;
  if (peer.received == 1) {
    peer.lastSeq = seq;
    return 0;
  }
  uint32_t expected = peer.lastSeq + 1;
  if (seq < expected) {
    peer.late++;
    return -1;
  }
  peer.lost += seq - expected;
  peer.lastSeq = seq;
  return seq - expected;
}

size_t airtimeUs(size_t bytes) {
  const RateOption *r = findRate(settings.rateTenthMbps);
  float mbps = (r ? r->tenthMbps : 10) / 10.0f;
  int preambleUs = r && r->mode == WIFI_PHY_MODE_11B ? 192 : 26;
  return preambleUs + (bytes + ESPNOW_FRAME_OVERHEAD_BYTES) * 8 / mbps;
}

// how much later than its schedule a packet arrived, relative to the previous one from the same sender
int32_t trackArrival(Peer &peer, int gap) {
  int64_t nowUs = esp_timer_get_time();
  int32_t late = 0;
  if (peer.lastArrivalUs && nowUs - peer.lastArrivalUs < ARRIVAL_RESYNC_US) {
    late = (int32_t)(nowUs - peer.lastArrivalUs) - (gap + 1) * (int32_t)PACKET_US;
    if (late > peer.maxLateUs) peer.maxLateUs = late;
  }
  peer.lastArrivalUs = nowUs;
  return max(late, (int32_t)0);
}

void latchLateness(Peer &peer, uint32_t lateUs) {
  if (lateUs > peer.lateLatchUs) peer.lateLatchUs = lateUs;
}

void relayPacket(const uint8_t *origin, const uint8_t *data, size_t len) {
  static RelayItem item;
  if (len > RELAY_MAX_PACKET_BYTES) {
    relayDrops++;
    return;
  }
  memcpy(item.origin, origin, 6);
  item.len = len;
  memcpy(item.data, data, len);
  if (xQueueSend(relayQueue, &item, 0) != pdTRUE) relayDrops++;
}

void resetPeerCounters() {
  for (Peer &p : peers) {
    p.received = p.lost = p.late = p.restarts = p.recovered = p.relayedReceived = 0;
  }
}

void onReceive(const esp_now_recv_info_t *recvInfo, const uint8_t *data, int len) {
  if (simLossPermille && esp_random() % 1000 < simLossPermille) return;
  if (blockActive && memcmp(recvInfo->src_addr + 4, blockedTail, 2) == 0) return;
  if (resetStatsRequested) {
    resetPeerCounters();
    resetStatsRequested = false;
  }
  if (len < (int)sizeof(PacketHeader)) {
    rxInvalid++;
    return;
  }
  PacketHeader header;
  memcpy(&header, data, sizeof(header));
  if (header.magic != PACKET_MAGIC) {
    rxInvalid++;
    return;
  }
  // magic and version keep their offsets across protocol versions, so a mismatch can always be read
  if (header.version != ownProtocolVersion) {
    rxWrongVersion++;
    if (header.version > ownProtocolVersion) newerFirmwareSeenMs = millis() | 1;
    else olderFirmwareSeenMs = millis() | 1;
    return;
  }

  // a relayed packet speaks for its origin, whose MAC rides right after the header
  bool relayed = header.flags & PACKET_FLAG_RELAYED;
  if (relayed && len < (int)sizeof(PacketHeader) + 6) {
    rxInvalid++;
    return;
  }
  const uint8_t *origin = relayed ? data + sizeof(PacketHeader) : recvInfo->src_addr;
  if (memcmp(origin, ownMac, 6) == 0) return;

  uint32_t now = millis();
  int slot = findOrAddPeer(origin, now);
  if (slot < 0) {
    rxPeerTableFull++;
    return;
  }
  Peer &peer = peers[slot];
  if (!peer.present || header.session != peer.session) {
    peer.present = true;
    peer.announced = false;
    peer.joinedMs = now;
    pendingJoins++;
  }
  // a new session means the sender rebooted: its sequence starts over and its decoder state is stale
  if (peer.received > 0 && header.session != peer.session) {
    peer.restarts++;
    peer.received = 0;
    peer.lastArrivalUs = 0;
    peerAudio[slot].resetPending = true;
  }
  peer.session = header.session;
  peer.lastSeenMs = now;
  if (!relayed) {
    peer.rssi = recvInfo->rx_ctrl->rssi;
    peer.rssiEma = peer.rssiEma == 0 ? peer.rssi : peer.rssiEma + (peer.rssi - peer.rssiEma) * RSSI_SMOOTHING;
    peer.directReceived++;
  }
  int gap = trackSequence(peer, header.seq);
  if (gap < 0) return;
  if (relayed) peer.relayedReceived++;

  size_t redundantSegment = header.frames * header.redundantBytes;
  size_t mainSegment = header.frames * header.frameBytes;
  bool keepalive = header.frames == 0;
  bool sane = keepalive ? (relayed || (header.flags & PACKET_FLAG_REPORT))
                        : header.frameBytes >= LC3_MIN_FRAME_BYTES && header.frameBytes <= LC3_MAX_FRAME_BYTES &&
                              mainSegment <= MAX_SEGMENT_BYTES && redundantSegment <= MAX_SEGMENT_BYTES &&
                              (header.depth == 0 || header.redundantBytes >= LC3_MIN_FRAME_BYTES);
  size_t payloadStart = sizeof(PacketHeader) + (relayed ? 6 : 0);
  size_t audioEnd = payloadStart + header.depth * redundantSegment + mainSegment;
  size_t expected = audioEnd;
  if (sane && relayed && (header.flags & (PACKET_FLAG_REPORT | PACKET_FLAG_CONFIG))) sane = false;
  if (sane && (header.flags & PACKET_FLAG_REPORT)) {
    expected = (int)audioEnd < len ? audioEnd + 1 + data[audioEnd] * REPORT_ENTRY_BYTES : len + 1;
  }
  size_t configStart = expected;
  if (header.flags & PACKET_FLAG_CONFIG) expected += sizeof(GroupConfig);
  if (!sane || len != (int)expected) {
    rxInvalid++;
    return;
  }
  if (header.flags & PACKET_FLAG_REPORT) parseReport(peer, data + audioEnd + 1, data[audioEnd], now);
  if (header.flags & PACKET_FLAG_CONFIG) receiveConfig(data + configStart);
  if (!relayed && peer.relaying) relayPacket(origin, data, audioEnd);
  // a keepalive only says the unit is still here; its timing must not feed the audio jitter estimate
  if (keepalive) {
    peer.lastArrivalUs = 0;
    return;
  }
  peer.lastAudioMs = now;
  int32_t jitterUs;
  if (relayed) {
    // a relay holds copies until its own slot, so they can arrive up to a packet interval after the original
    jitterUs = PACKET_US + airtimeUs(len) + SLOT_MARGIN_US;
  } else {
    jitterUs = trackArrival(peer, gap);
    // the callback fires after the whole frame was on air, so step back by its airtime to find when it was sent
    peer.phaseUs = packetPhase(esp_timer_get_time() - (int64_t)airtimeUs(len) - SLOT_RX_DELAY_US);
    peer.phaseValid = true;
  }
  const uint8_t *payload = data + payloadStart;
  int recover = min(gap, (int)header.depth);
  // packets lost beyond what redundancy covers are synthesised by the decoder's own concealment,
  // which keeps its state in step; longer outages are left to the jitter buffer instead
  int unrecoverable = gap - recover;
  if (unrecoverable > 0 && unrecoverable <= MAX_PLC_PACKETS) enqueueConcealment(slot, unrecoverable * header.frames);
  // redundant segments are oldest first; replay only the ones covering packets that never arrived
  for (int k = recover; k > 0; k--) {
    enqueueFrames(slot, payload + (header.depth - k) * redundantSegment, header.frames, header.redundantBytes);
  }
  peer.recovered += recover;
  // recovered or concealed audio arrives one packet interval late per packet it replaces; a longer outage
  // is a resync, and remembering it as lateness would park the buffer at its maximum for half a minute
  bool outage = gap > header.depth + MAX_PLC_PACKETS;
  latchLateness(peer, (outage ? 0 : gap * PACKET_US) + jitterUs);
  enqueueFrames(slot, payload + header.depth * redundantSegment, header.frames, header.frameBytes);
}

bool setupEspNow() {
  WiFi.useStaticBuffers(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, ownMac);
  ownSession = esp_random();

  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onReceive);
  esp_now_get_version(&espnowVersion);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) return false;
  if (!applyRate()) {
    settings.rateTenthMbps = 10;
    applyRate();
  }
  return true;
}
