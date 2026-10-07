#pragma once

#include <Preferences.h>
#include <algorithm>
#include <WiFi.h>
#include <base64.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#include "AudioTools.h"
#include "lc3.h"

constexpr uint8_t WIFI_CHANNEL = 1;
constexpr uint16_t PACKET_MAGIC = 0x4942;
constexpr uint8_t PROTOCOL_VERSION = 5;
constexpr int MAX_PACKET_BYTES = ESP_NOW_MAX_DATA_LEN_V2;
constexpr int MAX_SEGMENT_BYTES = 256;
constexpr uint32_t TX_JITTER_US = 300;
constexpr int32_t SLOT_MARGIN_US = 500;
constexpr int ESPNOW_FRAME_OVERHEAD_BYTES = 43;
constexpr uint32_t SLOT_CHECK_MS = 500;
constexpr int32_t SLOT_RX_DELAY_US = 150;
constexpr int MAX_REDUNDANCY = 2;
constexpr uint8_t PACKET_FLAG_REPORT = 1 << 0;
constexpr uint8_t PACKET_FLAG_RELAYED = 1 << 1;
constexpr uint8_t PACKET_FLAG_CONFIG = 1 << 2;
constexpr int CONFIG_EVERY_PACKETS = 50;
constexpr uint8_t REPORT_RELAY_BIT = 0x80;
constexpr float REPORT_MAX_LOSS_PCT = 63.5f;
constexpr float RELAY_NEED_LOSS_PCT = 20.0f;
constexpr float RELAY_LINK_MAX_LOSS_PCT = 10.0f;
constexpr int RELAY_SOURCE_MIN_RSSI = -88;
constexpr uint32_t RELAY_HOLD_MS = 3000;
constexpr int RELAY_QUEUE_DEPTH = 8;
constexpr int RELAY_MAX_PACKET_BYTES = 512;
constexpr int REPORT_EVERY_PACKETS = 10;
constexpr int REPORT_ENTRY_BYTES = 5;
constexpr uint32_t REPORT_FRESH_MS = 2000;
constexpr float REPORT_LOSS_SMOOTHING = 0.1f;
constexpr uint32_t ADAPT_INTERVAL_MS = 250;
constexpr uint32_t ADAPT_UPGRADE_HOLD_MS = 10000;
// the codec level follows signal strength, which falls steadily with distance; loss at short range is
// collisions and interference that the fixed redundancy already absorbs, so only a collapse counts
constexpr int ADAPT_GOOD_ENTER_RSSI = -75;
constexpr int ADAPT_GOOD_EXIT_RSSI = -78;
constexpr int ADAPT_POOR_ENTER_RSSI = -85;
constexpr int ADAPT_POOR_EXIT_RSSI = -82;
constexpr float ADAPT_COLLAPSE_LOSS_PCT = 10.0f;
// long enough for a relay to take over a listener whose direct link just failed
constexpr uint32_t ADAPT_COLLAPSE_HOLD_MS = 1500;
constexpr float RSSI_SMOOTHING = 0.1f;
constexpr float AIRTIME_BUDGET = 0.6f;
constexpr uint32_t PEER_AUDIO_ACTIVE_MS = 1000;
constexpr int DEFAULT_REDUNDANCY = 2;

constexpr int SAMPLE_RATE = 16000;
constexpr int LC3_FRAME_US = 10000;
constexpr int SAMPLES_PER_FRAME = SAMPLE_RATE * LC3_FRAME_US / 1000000;
constexpr int FRAMES_PER_PACKET = 2;
constexpr int DEFAULT_LC3_KBPS = 32;
constexpr int DEFAULT_REDUNDANT_KBPS = 16;
constexpr int MIN_LC3_KBPS = 16;
constexpr int MAX_LC3_KBPS = 64;
constexpr int MAX_PLC_PACKETS = 3;
constexpr int16_t SINE_AMPLITUDE = 12000;
constexpr uint32_t FRAME_US = LC3_FRAME_US;
constexpr uint32_t PACKET_US = FRAME_US * FRAMES_PER_PACKET;
// AudioTools sizes each DMA descriptor as buffer_size * buffer_count and the driver uses 6 of them,
// so 64-sample descriptors keep the output buffer at 24 ms
constexpr int I2S_DMA_SAMPLES = 64;

constexpr int MAX_PEERS = 8;
constexpr int RX_QUEUE_DEPTH = 32;
constexpr uint32_t PEER_ACTIVE_MS = 1000;
constexpr uint32_t PEER_REUSE_MS = 5000;
// a silence longer than this is an outage, not jitter: its length must not grow the jitter buffer
constexpr uint32_t ARRIVAL_RESYNC_US = 500000;

constexpr int JITTER_RING_SAMPLES = 3072;
constexpr float JITTER_BASE_TARGET_SAMPLES = 2.5f * SAMPLES_PER_FRAME;
constexpr float JITTER_MAX_TARGET_SAMPLES = 8 * SAMPLES_PER_FRAME;
constexpr float JITTER_HARD_SKIP_MARGIN = 5 * SAMPLES_PER_FRAME;
// late-arrival memory halves every ~10 s, so the buffer shrinks back once the link is clean again
constexpr float LATE_ENVELOPE_DECAY = 0.99945f;
constexpr float JITTER_LEVEL_SMOOTHING = 0.02f;
constexpr float RESAMPLE_DEADBAND = 64;
constexpr float RESAMPLE_SAMPLES_PER_STEP = 64;
constexpr int RESAMPLE_MAX_STEP = 1;
constexpr int MAX_CONCEAL_FRAMES = 3;
constexpr int FADE_IN_SAMPLES = 64;
constexpr uint32_t PEER_REJOIN_MS = 3000;
constexpr float CHIME_AMPLITUDE = 7000;
// after the startup chime the unit listens quietly for a moment, then announces how many others it found
constexpr uint32_t STARTUP_PHASE_MS = 1200;
constexpr int MAX_ANNOUNCE_NOTES = 24;
constexpr uint32_t ALERT_GRACE_MS = 5000;
constexpr uint32_t ALERT_REPEAT_MS = 30000;
constexpr uint32_t VERSION_SEEN_MS = 3000;
constexpr int CHIME_ENVELOPE_SAMPLES = 80;
constexpr int GLITCH_THRESHOLD = 5000;
constexpr uint32_t MIXER_LATE_US = 15000;

constexpr float HIGHPASS_HZ = 180.0f;
constexpr float LIMITER_THRESHOLD = 24000.0f;
constexpr int EQ_BANDS = 5;

constexpr uint32_t SERIAL_BAUD = 460800;
constexpr uint32_t STATS_INTERVAL_MS = 1000;
constexpr uint32_t SCOPE_INTERVAL_MS = 500;
constexpr int SCOPE_SAMPLES = 256;
constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 2000;

constexpr uint8_t HOST_FRAME_MAGIC_1 = 0xA5;
constexpr uint8_t HOST_FRAME_MAGIC_2 = 0x5A;
constexpr uint8_t HOST_FRAME_MAGIC_ULAW = 0x5B;
constexpr int HOST_FRAME_MAX_BYTES = 2048;
constexpr int HOST_RING_SAMPLES = 4096;
constexpr int HOST_PRIME_SAMPLES = 6 * SAMPLES_PER_FRAME;
constexpr int HOST_LOW_SAMPLES = 3 * SAMPLES_PER_FRAME;
constexpr int HOST_HIGH_SAMPLES = 10 * SAMPLES_PER_FRAME;

const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

AudioInfo info(SAMPLE_RATE, 1, 16);

// peer timestamps are written by the WiFi task on the other core, so one can be newer than a `now`
// read a moment earlier; plain unsigned subtraction would then wrap to ~49 days
inline uint32_t msSince(uint32_t then, uint32_t now) {
  int32_t d = (int32_t)(now - then);
  return d > 0 ? d : 0;
}
