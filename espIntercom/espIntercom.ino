/**
 * @file example-serial-receive.ino
 * @author Phil Schatzmann
 * @brief Receiving audio via ESPNow and decoding data to I2S 
 * @version 0.1
 * @date 2022-03-09
 * 
 * @copyright Copyright (c) 2022
 */

#include "AudioTools.h"
#include "AudioTools/Communication/ESPNowStream.h"
#include "AudioTools/AudioCodecs/CodecSBC.h"

const char *peers[] = {"A8:48:FA:0B:93:01"};

ESPNowStream now;

AudioInfo info(16000, 1, 16);

//TX
SineWaveGenerator<int16_t> sineWave(32000);     // subclass of SoundGenerator with max amplitude of 32000
GeneratedSoundStream<int16_t> sound(sineWave);  // Stream generated from sine wave
EncodedAudioStream encoder(&now, new SBCEncoder());   // encode and write to ESP-now
StreamCopy copier(encoder, sound);  // copies sound into i2s

//RX
CsvOutput<int16_t> out(Serial);
BufferRTOS<uint8_t> buffer1(1024 * 5);  // fast synchronized buffer
QueueStream<uint8_t> queue1(buffer1);  // stream from espnow
BufferRTOS<uint8_t> buffer2(1024 * 5);  // fast synchronized buffer
QueueStream<uint8_t> queue2(buffer1);  // stream from espnow
OutputMixer<int16_t> mixer(out, 2);  // output mixer with 2 outputs mixing to AudioBoardStream 
EncodedAudioStream decoder1(&mixer, new SBCDecoder(256)); // decode and write to I2S - ESP Now is limited to 256 bytes
EncodedAudioStream decoder2(&mixer, new SBCDecoder(256)); // decode and write to I2S - ESP Now is limited to 256 bytes

void recieveCallback(const esp_now_recv_info *info, const uint8_t *data, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           info->src_addr[0], info->src_addr[1], info->src_addr[2], info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  if (strcmp(macStr, peers[0])) {
    // write audio data to the queue
    decoder1.write(data, len);
  }
  if (strcmp(macStr, peers[1])) {
    // write audio data to the queue
    decoder2.write(data, len);
  }
  //Do we have to flush mixer if not all are connected?
}


void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // setup esp-now
  auto cfg = now.defaultConfig();
  now.setReceiveCallback(recieveCallback);
  cfg.mac_address = "A8:48:FA:0B:93:03";
  cfg.delay_after_failed_write_ms = 0;
  cfg.use_send_ack = false;
  cfg.write_retry_count = 0;
  cfg.rate = WIFI_PHY_RATE_MCS7_LGI;
  now.begin(cfg);
  now.addPeers(peers);


  out.begin(info);
  mixer.begin();

  queue1.begin();
  queue2.begin();

  // start decoder
  decoder1.begin();
  decoder2.begin();

  sineWave.begin(info, N_B4);
  encoder.begin(info);

  Serial.println("Receiver started...");
}

void loop() { 
  copier.copy();
}