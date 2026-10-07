#pragma once

#include "Settings.h"

template <int CAPACITY>
class SampleRing {
 public:
  size_t available() const { return count; }

  void write(const int16_t *samples, size_t n) {
    for (size_t i = 0; i < n; i++) {
      if (count == CAPACITY) {
        tail = (tail + 1) % CAPACITY;
        count--;
        overflows++;
      }
      data[head] = samples[i];
      head = (head + 1) % CAPACITY;
      count++;
    }
  }

  void read(int16_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
      out[i] = data[tail];
      tail = (tail + 1) % CAPACITY;
    }
    count -= n;
  }

  void skip(size_t n) {
    n = min(n, count);
    tail = (tail + n) % CAPACITY;
    count -= n;
  }

  void clear() { head = tail = count = 0; }

  uint32_t overflows = 0;

 private:
  int16_t data[CAPACITY];
  size_t head = 0;
  size_t tail = 0;
  size_t count = 0;
};

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
    b0 = nb0 / a0;
    b1 = nb1 / a0;
    b2 = nb2 / a0;
    a1 = na1 / a0;
    a2 = na2 / a0;
  }

  void highpass(float fs, float f0, float q) {
    float w = 2 * PI * f0 / fs, alpha = sinf(w) / (2 * q), c = cosf(w);
    set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
  }

  void peaking(float fs, float f0, float q, float gainDb) {
    float a = powf(10, gainDb / 40), w = 2 * PI * f0 / fs, alpha = sinf(w) / (2 * q), c = cosf(w);
    set(1 + alpha * a, -2 * c, 1 - alpha * a, 1 + alpha / a, -2 * c, 1 - alpha / a);
  }
};

Biquad highpass;
Biquad eqBands[EQ_BANDS];
Biquad stagedBands[EQ_BANDS];
volatile bool eqStaged = false;

// computed in the command handler, swapped in by the mixer between frames so a change never lands mid-frame
void setupVoiceEq() {
  highpass.highpass(info.sample_rate, HIGHPASS_HZ, 0.707f);
  for (int i = 0; i < EQ_BANDS; i++) {
    const EqBand &b = settings.eq[i];
    stagedBands[i].peaking(info.sample_rate, b.hz, b.q10 / 10.0f, b.db);
  }
  eqStaged = true;
}

void applyStagedEq() {
  if (!eqStaged) return;
  for (int i = 0; i < EQ_BANDS; i++) {
    eqBands[i].b0 = stagedBands[i].b0;
    eqBands[i].b1 = stagedBands[i].b1;
    eqBands[i].b2 = stagedBands[i].b2;
    eqBands[i].a1 = stagedBands[i].a1;
    eqBands[i].a2 = stagedBands[i].a2;
  }
  eqStaged = false;
}

float applyVoiceEq(float v) {
  v = highpass.process(v);
  for (Biquad &b : eqBands) v = b.process(v);
  return v;
}

// soft knee above the threshold so summed peers round off instead of hard clipping
float softLimit(float x) {
  float mag = fabsf(x);
  if (mag <= LIMITER_THRESHOLD) return x;
  float headroom = INT16_MAX - LIMITER_THRESHOLD;
  float limited = LIMITER_THRESHOLD + headroom * tanhf((mag - LIMITER_THRESHOLD) / headroom);
  return x < 0 ? -limited : limited;
}
