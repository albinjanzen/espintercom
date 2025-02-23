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
//#include "AudioTools/Communication/ESPNowStream.h"
#include "ESPNowStreamC.h"
#include "AudioTools/AudioCodecs/CodecSBC.h"


const char *peers[] = { "A8:48:FA:0B:93:02"};

ESPNowStreamC now;

AudioInfo info(16000, 1, 16);

//TX
SineWaveGenerator<int16_t> sineWave(32000);         // subclass of SoundGenerator with max amplitude of 32000
GeneratedSoundStream<int16_t> sound(sineWave);      // Stream generated from sine wave
EncodedAudioStream encoder(&now, new SBCEncoder());  // encode and write to ESP-now
StreamCopy copier(encoder, sound);                  // copies sound into i2s

//RX
I2SStream out;

BufferRTOS<uint8_t> buffer1(1024);                    // fast synchronized buffer
QueueStream<uint8_t> queue1(buffer1);                     // stream from espnow

EncodedAudioStream decoder1(&out, new SBCDecoder(256));  // decode and write to I2S - ESP Now is limited to 256 bytes


void recieveCallback(const esp_now_recv_info *info, const uint8_t *data, int len) {
  decoder1.write(data, len);
  //Do we have to flush mixer if not all are connected?
}


void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

  // setup esp-now
  auto cfg = now.defaultConfig();
  now.setReceiveCallback(recieveCallback);
  cfg.mac_address = "A8:48:FA:0B:93:01";
  cfg.delay_after_failed_write_ms = 0;
  cfg.use_send_ack = false;
  cfg.write_retry_count = 0;
  //cfg.rate = WIFI_PHY_RATE_MCS7_LGI;
  cfg.buffer_count = 5;
  now.begin(cfg);
  now.addPeers(peers);


  queue1.begin();

  // start decoder
  decoder1.begin(info);

  auto config = out.defaultConfig(TX_MODE);
  out.begin(config);

  sineWave.begin(info, N_B4);
  encoder.begin(info);

  Serial.println("Receiver started...");
}

void loop() {
  copier.copy();
}