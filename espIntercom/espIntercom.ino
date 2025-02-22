/**
 * @file example-serial-send.ino
 * @author Phil Schatzmann
 * @brief Sending encoded audio over ESPNow
 * @version 0.1
 * @date 2022-03-09
 *
 * @copyright Copyright (c) 2022
 */

#include "AudioTools.h"
#include "AudioTools/Communication/ESPNowStream.h"
#include "AudioTools/AudioCodecs/CodecSBC.h"
#include "AudioTools/AudioLibs/Concurrency.h"

ESPNowStream now;

AudioInfo info(32000, 1, 16);
SineWaveGenerator<int16_t> sineWave(32000);     // subclass of SoundGenerator with max amplitude of 32000
GeneratedSoundStream<int16_t> sound(sineWave);  // Stream generated from sine wave
SBCEncoder sbc;
EncodedAudioStream encoder(&now, &sbc);   // encode and write to ESP-now
StreamCopy copierSource(encoder, sound);  // copies sound into i2s

CsvOutput<int16_t> out(Serial);
BufferRTOS<uint8_t> buffer(1024 * 10);                  // fast synchronized buffer
QueueStream<uint8_t> queue(buffer);                     // stream from espnow
EncodedAudioStream decoder(&queue, new SBCDecoder(256));  // decode and write to I2S - ESP Now is limited to 256 bytes
StreamCopy copierSink(out, queue);


void recieveCallback(const esp_now_recv_info *info, const uint8_t *data, int len) {
  decoder.write(data, len);
}

const char *peers[] = { "A8:48:FA:0B:93:02" };

// tasks
Task sendTask("send-audio", 4096, 1, 0);
Task receiveTask("receive-audio", 4096, 1, 1);

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // setup esp-now
  now.setReceiveCallback(recieveCallback);
  now.addPeers(peers);
  auto cfg = now.defaultConfig();
  cfg.mac_address = "A8:48:FA:0B:93:01";
  now.begin(cfg);

  // Setup sine wave
  sineWave.begin(info, N_B4);

  // start encoder
  encoder.begin(info);

  queue.begin(info);

  // start I2S
  Serial.println("starting I2S...");
  auto config = out.defaultConfig(TX_MODE);
  out.begin(info);

  // start decoder
  decoder.begin(info);

  // Start audio tasks
  sendTask.begin([]() {
    copierSource.copy();
  });

  receiveTask.begin([]() {
    copierSink.copy();
  });

  Serial.println("Started...");
}

void loop() {
}
