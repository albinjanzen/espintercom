#pragma once

#include "Dsp.h"
#include "Scope.h"

// frameBytes 0 means the frames were lost for good and the decoder should conceal them
struct RxItem {
  uint8_t slot;
  uint8_t generation;
  uint8_t frames;
  uint16_t frameBytes;
  uint8_t data[MAX_SEGMENT_BYTES];
};

struct Peer {
  bool used;
  volatile bool present;
  // held silent until the sound announcing this unit has played, so the tone comes before the voice
  volatile bool announced;
  uint32_t joinedMs;
  uint8_t mac[6];
  uint16_t session;
  uint32_t lastSeenMs;
  uint32_t lastSeq;
  uint32_t received;
  uint32_t lost;
  uint32_t late;
  uint32_t restarts;
  uint32_t recovered;
  int64_t lastArrivalUs;
  volatile int32_t maxLateUs;
  volatile uint32_t lateLatchUs;
  volatile int32_t phaseUs;
  volatile bool phaseValid;
  uint32_t lastAudioMs;
  // reception counted from direct packets only, so a relay does not hide that the direct link is bad
  uint32_t directReceived;
  uint32_t relayedReceived;
  uint32_t reportSeq;
  uint32_t reportDirect;
  float directLossEma;
  bool relaying;
  uint32_t relayHoldUntilMs;
  // this peer's latest reception report: which units it hears, how well, and which it relays
  struct __attribute__((packed)) ReportEntry {
    uint8_t mac3[3];
    uint8_t loss;
    int8_t rssi;
  } entries[MAX_PEERS];
  uint8_t entryCount;
  volatile uint32_t reportMs;
  volatile float aboutMeLossPct;
  volatile int8_t aboutMeRssi;
  volatile uint32_t aboutMeMs;
  int8_t rssi;
  float rssiEma;
};

using PeerRing = SampleRing<JITTER_RING_SAMPLES>;

// owned by the mixer task, except resetPending which the receive callback sets
struct PeerAudio {
  void reset() {
    ring.clear();
    decoder = lc3_setup_decoder(LC3_FRAME_US, SAMPLE_RATE, 0, &decoderMemory);
    primed = false;
    fadeIn = true;
    concealed = 0;
    concealDebt = 0;
    lateEnvelope = 0;
    target = JITTER_BASE_TARGET_SAMPLES;
  }

  lc3_decoder_mem_16k_t decoderMemory;
  lc3_decoder_t decoder = nullptr;
  PeerRing ring;
  int16_t frame[SAMPLES_PER_FRAME] = {};
  int16_t input[SAMPLES_PER_FRAME + RESAMPLE_MAX_STEP + 1] = {};
  bool primed = false;
  bool fadeIn = true;
  int concealed = 0;
  size_t concealDebt = 0;
  float avgLevel = 0;
  float lateEnvelope = 0;
  float target = JITTER_BASE_TARGET_SAMPLES;
  volatile bool resetPending = true;
  // bumped when the slot is given to a new unit, so audio still queued for the old one is dropped
  volatile uint8_t generation = 0;
  volatile bool mixing = false;
  volatile uint16_t bufferedSamples = 0;
  uint32_t underruns = 0;
  uint32_t decoderConcealed = 0;
  uint32_t decodeErrors = 0;
  uint32_t concealedFrames = 0;
  uint32_t debtSkipped = 0;
  uint32_t driftDrops = 0;
  uint32_t driftInserts = 0;
};

class OutputTap : public Print {
 public:
  explicit OutputTap(Print &out) : out(out) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    rxScope.capture((const int16_t *)data, len / sizeof(int16_t));
    return out.write(data, len);
  }

 private:
  Print &out;
};

SineWaveGenerator<int16_t> testWave(SINE_AMPLITUDE);
I2SStream out;
OutputTap outputTap(out);
VolumeStream volume(outputTap);

QueueHandle_t rxQueue;
Peer peers[MAX_PEERS];
// heap allocated: eight jitter buffers and decoders do not fit the static DRAM segment
PeerAudio *peerAudio;

enum FrameEvent : uint16_t {
  EV_CONCEAL = 1 << 0,
  EV_DEBT_SKIP = 1 << 1,
  EV_PRIME = 1 << 2,
  EV_STRETCH = 1 << 3,
  EV_UNDERRUN = 1 << 4,
  EV_HARD_SKIP = 1 << 5,
  EV_CHIME = 1 << 6,
  EV_FADE_IN = 1 << 7,
  EV_LATE_MIXER = 1 << 8,
  EV_DECODER_RESET = 1 << 9,
  EV_BURST = 1 << 10,
};

struct GlitchEvent {
  uint32_t ms;
  uint16_t events;
  int16_t jump;
  uint32_t periodUs;
  uint8_t mixed;
};

// what happened while building the current output frame, so a detected pop can be traced to its cause
uint16_t frameEvents = 0;
QueueHandle_t glitchQueue;
volatile uint32_t glitchCount = 0;

uint32_t bootMs = 0;
volatile uint8_t pendingJoins = 0;
volatile uint8_t pendingLeaves = 0;

bool inStartupPhase() { return msSince(bootMs, millis()) < STARTUP_PHASE_MS; }

volatile bool localTestTone = false;
volatile uint32_t rxQueueDrops = 0;
volatile uint32_t clippedSamples = 0;
volatile uint8_t mixedPeers = 0;
volatile uint32_t mixerBusyUs = 0;
volatile uint32_t decodeUsTotal = 0;
volatile uint32_t decodeCount = 0;
volatile uint32_t decodeUsMax = 0;
volatile uint32_t mixerFrames = 0;
volatile uint32_t mixerPeriodMaxUs = 0;
volatile uint32_t mixerPeriodMinUs = UINT32_MAX;

void applyVolume() { volume.setVolume(settings.volume / 100.0f); }

void setupAudioOutput() {
  peerAudio = new PeerAudio[MAX_PEERS];
  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxItem));
  glitchQueue = xQueueCreate(16, sizeof(GlitchEvent));
  auto cfg = out.defaultConfig(TX_MODE);
  cfg.copyFrom(info);
  cfg.pin_bck = settings.pinBck;
  cfg.pin_ws = settings.pinWs;
  cfg.pin_data = settings.pinData;
  cfg.buffer_size = I2S_DMA_SAMPLES * sizeof(int16_t);
  cfg.buffer_count = 1;
  out.begin(cfg);
  volume.begin(info);
  applyVolume();
}

void enqueueFrames(int slot, const uint8_t *data, int frames, int frameBytes) {
  RxItem item;
  item.slot = slot;
  item.generation = peerAudio[slot].generation;
  item.frames = frames;
  item.frameBytes = frameBytes;
  memcpy(item.data, data, frames * frameBytes);
  if (xQueueSend(rxQueue, &item, 0) != pdTRUE) rxQueueDrops++;
}

void enqueueConcealment(int slot, int frames) {
  RxItem item;
  item.slot = slot;
  item.generation = peerAudio[slot].generation;
  item.frames = frames;
  item.frameBytes = 0;
  if (xQueueSend(rxQueue, &item, 0) != pdTRUE) rxQueueDrops++;
}

void resetPeerAudioIfPending(PeerAudio &audio) {
  if (!audio.resetPending) return;
  frameEvents |= EV_DECODER_RESET;
  audio.reset();
  audio.resetPending = false;
}

void decodePending() {
  static int16_t pcm[SAMPLES_PER_FRAME];
  RxItem item;
  int decoded = 0;
  while (xQueueReceive(rxQueue, &item, 0) == pdTRUE) {
    if (!peers[item.slot].announced) continue;
    if (++decoded > 3) frameEvents |= EV_BURST;
    PeerAudio &audio = peerAudio[item.slot];
    if (item.generation != audio.generation) continue;
    resetPeerAudioIfPending(audio);
    for (int f = 0; f < item.frames; f++) {
      const uint8_t *in = item.frameBytes ? item.data + f * item.frameBytes : nullptr;
      uint32_t t0 = micros();
      int result = lc3_decode(audio.decoder, in, item.frameBytes, LC3_PCM_FORMAT_S16, pcm, 1);
      uint32_t took = micros() - t0;
      decodeUsTotal += took;
      decodeCount++;
      if (took > decodeUsMax) decodeUsMax = took;
      if (result < 0) {
        audio.decodeErrors++;
        continue;
      }
      if (!in) audio.decoderConcealed++;
      audio.ring.write(pcm, SAMPLES_PER_FRAME);
    }
  }
}

// plays the last frame mirrored so it continues from the final sample, fading to silence, then holds
// silence for a few frames so a short burst loss keeps the buffer timing instead of re-priming
bool concealFrame(PeerAudio &audio, int32_t *mix) {
  if (audio.concealed >= MAX_CONCEAL_FRAMES) {
    audio.primed = false;
    audio.underruns++;
    frameEvents |= EV_UNDERRUN;
    return false;
  }
  frameEvents |= EV_CONCEAL;
  if (audio.concealed == 0) {
    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
      float ramp = 1.0f - (float)(i + 1) / SAMPLES_PER_FRAME;
      mix[i] += audio.frame[SAMPLES_PER_FRAME - 1 - i] * ramp;
    }
  }
  audio.concealed++;
  audio.concealedFrames++;
  audio.concealDebt += SAMPLES_PER_FRAME;
  audio.fadeIn = true;
  return true;
}

// audio that shows up after its gap was already concealed would add latency, so the concealed
// stretch is dropped as long as that leaves the buffer at its target
size_t payConcealDebt(PeerAudio &audio, size_t level) {
  size_t excess = level > audio.target ? level - (size_t)audio.target : 0;
  size_t skip = min(audio.concealDebt, excess);
  if (skip) frameEvents |= EV_DEBT_SKIP;
  audio.ring.skip(skip);
  audio.debtSkipped += skip;
  audio.concealDebt = 0;
  return level - skip;
}

// reads slightly more or fewer input samples than one frame and interpolates them to exactly one frame,
// so clock drift and jitter recovery bend the pitch by a percent instead of clicking
void readResampled(PeerAudio &audio, size_t inputCount) {
  int16_t *x = audio.input;
  x[0] = audio.frame[SAMPLES_PER_FRAME - 1];
  audio.ring.read(x + 1, inputCount);
  if (inputCount == SAMPLES_PER_FRAME) {
    memcpy(audio.frame, x + 1, SAMPLES_PER_FRAME * sizeof(int16_t));
    return;
  }
  float step = (float)inputCount / SAMPLES_PER_FRAME;
  for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
    float pos = (i + 1) * step;
    int idx = (int)pos;
    float frac = pos - idx;
    audio.frame[i] = idx >= (int)inputCount ? x[inputCount] : x[idx] + (x[idx + 1] - x[idx]) * frac;
  }
}

size_t resampleInputCount(PeerAudio &audio, size_t level) {
  float error = audio.avgLevel - audio.target;
  if (fabsf(error) < RESAMPLE_DEADBAND) return SAMPLES_PER_FRAME;
  int step = constrain((int)roundf(error / RESAMPLE_SAMPLES_PER_STEP), -RESAMPLE_MAX_STEP, RESAMPLE_MAX_STEP);
  size_t count = SAMPLES_PER_FRAME + step;
  return count <= level ? count : SAMPLES_PER_FRAME;
}

// the buffer target follows the worst recent lateness of this sender, including audio that only
// arrived through redundancy, so lossy links get more buffer and clean links get less latency
void updateTarget(Peer &peer, PeerAudio &audio) {
  uint32_t lateUs = peer.lateLatchUs;
  peer.lateLatchUs = 0;
  float lateSamples = lateUs * info.sample_rate / 1000000.0f;
  audio.lateEnvelope = max(audio.lateEnvelope * LATE_ENVELOPE_DECAY, lateSamples);
  audio.target = min(JITTER_BASE_TARGET_SAMPLES + audio.lateEnvelope, JITTER_MAX_TARGET_SAMPLES);
}

bool mixPeer(int slot, int32_t *mix) {
  PeerAudio &audio = peerAudio[slot];
  resetPeerAudioIfPending(audio);
  updateTarget(peers[slot], audio);
  size_t level = audio.ring.available();
  if (!audio.primed) {
    if (level < audio.target + SAMPLES_PER_FRAME / 2) return false;
    audio.primed = true;
    frameEvents |= EV_PRIME;
    audio.avgLevel = level;
    audio.concealed = 0;
  }
  if (level < SAMPLES_PER_FRAME) return concealFrame(audio, mix);
  audio.concealed = 0;
  if (audio.concealDebt) level = payConcealDebt(audio, level);

  if (level > audio.target + JITTER_HARD_SKIP_MARGIN) {
    frameEvents |= EV_HARD_SKIP;
    size_t keep = audio.target;
    audio.driftDrops += level - keep;
    audio.ring.skip(level - keep);
    level = keep;
    audio.avgLevel = level;
    audio.fadeIn = true;
  }
  audio.avgLevel += (level - audio.avgLevel) * JITTER_LEVEL_SMOOTHING;

  size_t inputCount = resampleInputCount(audio, level);
  if (inputCount != SAMPLES_PER_FRAME) frameEvents |= EV_STRETCH;
  if (inputCount > SAMPLES_PER_FRAME) audio.driftDrops += inputCount - SAMPLES_PER_FRAME;
  if (inputCount < SAMPLES_PER_FRAME) audio.driftInserts += SAMPLES_PER_FRAME - inputCount;
  readResampled(audio, inputCount);

  if (audio.fadeIn) {
    frameEvents |= EV_FADE_IN;
    for (int i = 0; i < FADE_IN_SAMPLES; i++) audio.frame[i] = audio.frame[i] * (float)i / FADE_IN_SAMPLES;
    audio.fadeIn = false;
  }
  for (int i = 0; i < SAMPLES_PER_FRAME; i++) mix[i] += audio.frame[i];
  return true;
}

struct ChimeNote {
  uint16_t hz;
  uint16_t ms;
};

const ChimeNote STARTUP_CHIME[] = {{660, 90}, {0, 40}, {880, 90}, {0, 40}, {1320, 160}, {0, 0}};
const ChimeNote JOIN_CHIME[] = {{880, 80}, {0, 30}, {1320, 140}, {0, 0}};
const ChimeNote LEAVE_CHIME[] = {{1320, 80}, {0, 30}, {880, 140}, {0, 0}};
const ChimeNote PIP = {988, 70};
const ChimeNote UPDATE_CHIME[] = {{300, 120}, {0, 60}, {200, 160}, {0, 140}, {300, 120}, {0, 60}, {200, 160}, {0, 0}};
// like an engaged phone line
const ChimeNote GROUP_FULL_CHIME[] = {{250, 350}, {0, 350}, {250, 350}, {0, 0}};
const ChimeNote PIP_GAP = {0, 110};

// short tone sequences mixed into the output; other tasks request one and the mixer plays it
class Chime {
 public:
  void request(const ChimeNote *notes) { pending = notes; }

  bool idle() const { return pending == nullptr && (note == nullptr || note->ms == 0); }

  bool addTo(int32_t *mix) {
    if (note == nullptr || note->ms == 0) {
      note = pending;
      pending = nullptr;
      sampleInNote = 0;
      if (note == nullptr) return false;
    }
    for (int i = 0; i < SAMPLES_PER_FRAME && note && note->ms; i++) {
      int noteSamples = note->ms * info.sample_rate / 1000;
      if (note->hz) {
        int fromEdge = min(sampleInNote, noteSamples - 1 - sampleInNote);
        float envelope = min(1.0f, (float)fromEdge / CHIME_ENVELOPE_SAMPLES);
        phase += 2 * PI * note->hz / info.sample_rate;
        if (phase > 2 * PI) phase -= 2 * PI;
        mix[i] += CHIME_AMPLITUDE * envelope * sinf(phase);
      }
      if (++sampleInNote >= noteSamples) {
        note++;
        sampleInNote = 0;
      }
    }
    return true;
  }

 private:
  const ChimeNote *volatile pending = nullptr;
  const ChimeNote *note = nullptr;
  int sampleInNote = 0;
  float phase = 0;
};

Chime chime;

// warnings decided by the transmit side, played by the mixer ahead of join and leave sounds
const ChimeNote *volatile pendingAlert = nullptr;

// one pip per unit found, played once after startup
const ChimeNote *composeStatus(int units) {
  static ChimeNote notes[MAX_ANNOUNCE_NOTES];
  int n = 0;
  for (int i = 0; i < units && n < MAX_ANNOUNCE_NOTES - 2; i++) {
    notes[n++] = PIP;
    notes[n++] = PIP_GAP;
  }
  notes[n] = {0, 0};
  return notes;
}

void announceDepartures() {
  uint32_t now = millis();
  for (Peer &p : peers) {
    if (p.used && p.present && msSince(p.lastSeenMs, now) > PEER_REJOIN_MS) {
      p.present = false;
      pendingLeaves++;
    }
  }
}

int presentPeers() {
  int n = 0;
  for (Peer &p : peers) n += p.used && p.present;
  return n;
}

uint8_t unannouncedMask() {
  uint8_t mask = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && peers[i].present && !peers[i].announced) mask |= 1 << i;
  }
  return mask;
}

// after startup one pip per unit already there replaces their join sounds; later each join and
// leave gets its own sound. A unit's audio is released only once the sound announcing it has played
void announceGroupChanges() {
  static bool statusDone = false;
  static uint8_t releaseMask = 0;
  if (!chime.idle()) return;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (releaseMask & (1 << i)) peers[i].announced = true;
  }
  releaseMask = 0;
  if (!statusDone) {
    if (inStartupPhase()) return;
    statusDone = true;
    pendingJoins = pendingLeaves = 0;
    int others = presentPeers();
    releaseMask = unannouncedMask();
    if (others) chime.request(composeStatus(others));
    return;
  }
  if (pendingAlert) {
    chime.request(pendingAlert);
    pendingAlert = nullptr;
    return;
  }
  if (pendingJoins) {
    pendingJoins--;
    releaseMask = unannouncedMask();
    chime.request(JOIN_CHIME);
  } else if (pendingLeaves) {
    pendingLeaves--;
    chime.request(LEAVE_CHIME);
  }
}

void playLocalTestBlock() {
  static int16_t block[SAMPLES_PER_FRAME];
  for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
    float v = testWave.readSample();
    block[i] = (int16_t)softLimit(settings.voiceEq ? applyVoiceEq(v) : v);
  }
  volume.write((uint8_t *)block, sizeof(block));
}

uint32_t trackMixerPeriod(uint32_t start) {
  static uint32_t lastStart = start;
  uint32_t period = start - lastStart;
  lastStart = start;
  if (period > mixerPeriodMaxUs) mixerPeriodMaxUs = period;
  if (period < mixerPeriodMinUs) mixerPeriodMinUs = period;
  mixerFrames++;
  return period;
}

// a smooth signal has a small second difference, so a large one marks a click in the output
void detectGlitch(const int16_t *frame, uint32_t periodUs, uint8_t mixed) {
  static int16_t prev1 = 0, prev2 = 0;
  int16_t worst = 0;
  for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
    int32_t jump = abs(frame[i] - 2 * prev1 + prev2);
    if (jump > worst) worst = min(jump, (int32_t)INT16_MAX);
    prev2 = prev1;
    prev1 = frame[i];
  }
  bool dip = frameEvents & (EV_CONCEAL | EV_UNDERRUN | EV_HARD_SKIP);
  if (worst < GLITCH_THRESHOLD && !dip) return;
  if (worst >= GLITCH_THRESHOLD) glitchCount++;
  GlitchEvent ev{millis(), frameEvents, worst, periodUs, mixed};
  xQueueSend(glitchQueue, &ev, 0);
}

void mixerTask(void *) {
  static int32_t mix[SAMPLES_PER_FRAME];
  static int16_t frame[SAMPLES_PER_FRAME];
  esp_task_wdt_add(nullptr);
  while (true) {
    esp_task_wdt_reset();
    applyStagedEq();
    if (localTestTone) {
      xQueueReset(rxQueue);
      playLocalTestBlock();
      continue;
    }
    uint32_t start = micros();
    frameEvents = 0;
    uint32_t period = trackMixerPeriod(start);
    if (period > MIXER_LATE_US) frameEvents |= EV_LATE_MIXER;
    decodePending();

    memset(mix, 0, sizeof(mix));
    uint8_t mixed = 0;
    for (int slot = 0; slot < MAX_PEERS; slot++) {
      bool active = peers[slot].used && mixPeer(slot, mix);
      peerAudio[slot].mixing = active;
      peerAudio[slot].bufferedSamples = peerAudio[slot].ring.available();
      if (active) mixed++;
    }
    mixedPeers = mixed;
    announceDepartures();
    announceGroupChanges();
    if (chime.addTo(mix)) frameEvents |= EV_CHIME;

    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
      float v = settings.playEnabled ? mix[i] : 0;
      if (settings.voiceEq) v = applyVoiceEq(v);
      if (fabsf(v) > LIMITER_THRESHOLD) clippedSamples++;
      frame[i] = (int16_t)softLimit(v);
    }
    detectGlitch(frame, period, mixed);
    mixerBusyUs = micros() - start;
    // blocks on the I2S DMA, which paces this loop at the sample rate
    volume.write((uint8_t *)frame, sizeof(frame));
  }
}
