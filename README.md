# ESP Intercom

A live, always on voice intercom for skydivers, built on plain ESP32 boards. Every unit that is powered on joins the group by itself and everyone hears everyone at the same time: no push to talk, no pairing, no base station. Commercial mesh intercoms (Cardo DMC, Sena Mesh) are the quality benchmark.

Range and speech clarity are the primary goals. Battery life is secondary, since a jump lasts minutes.

## Current state

Working and verified on three units at the desk:

| Area | State |
|---|---|
| Radio | ESP-NOW v2 broadcast, channel 1, 1 Mbps PHY rate for maximum range |
| Codec | LC3 at 16 kHz, 10 ms frames, 2 frames per 20 ms packet |
| Loss handling | 2 deep redundancy with low bitrate copies (16 kbps), LC3 concealment for longer gaps |
| Playback | One decoder per sender, mixing, adaptive jitter buffer, resampling for clock drift |
| Collisions | Self organising time slots: each unit moves its send moment into a free gap |
| Bitrate | Adaptive per sender from receiver reports, driven by signal strength (48, 32 or 16 kbps) |
| Mesh | One hop relay on demand: a unit forwards a sender that a neighbour cannot hear |
| Group settings | PHY rate and codec settings spread to every unit, last change wins |
| Sounds | Startup, status pips, join, leave, firmware update needed, group full |
| Sound quality | Voice EQ with soft limiter, tuned against a measured speaker response |
| Robustness | Session IDs, protocol versioning, watchdogs, reset reason, settings validation |

Measured at the desk with two to three units:

| Metric | Value |
|---|---|
| Air loss | about 0.2 to 0.5 %, nearly all recovered by redundancy |
| End to end latency | about 70 to 90 ms (adaptive) |
| Packet size / airtime | about 176 to 214 bytes / about 2 ms at 1 Mbps, room for about 8 units per cycle |
| CPU | core 0 about 58 % (mostly LC3 encode), core 1 about 15 to 25 % (decode and mix) |
| Free heap | about 66 KB with 8 peer slots |

Not done yet: the microphone. Units currently send a test sine, or radio streamed from the Mac through the debug UI.

## Hardware

- **Board:** AZ-Delivery NodeMCU ESP-32S (plain ESP32-D0WD-V3, 4 MB flash, CH340 USB serial). Not an ESP32-S3.
- **Amplifier:** MAX98357A I2S class D amp, VIN on 3.3 V (deliberate, to save battery), SD and GAIN unconnected.
- **Speaker:** small 8 Ω driver close to the ear. A measured EQ compensates its resonances.
- **Planned:** INMP441 I2S microphone; HW-373 (TP4056) charger with an LDO (ME6211 or RT9013) to 3.3 V.

Default I2S pins are BCK 14, WS 15, DATA 22. A board wired differently gets its pins over serial (`pins <bck> <ws> <data>`), which is saved on the board. The current speaker board uses BCK 15, WS 14, DATA 22.

## Repository layout

```
espIntercom/              Arduino sketch
  espIntercom.ino         setup() and loop()
  Config.h                constants and tuning parameters
  Settings.h              persisted settings, validation, deferred flash writes
  Dsp.h                   ring buffer, biquad EQ, soft limiter
  Scope.h                 waveform capture for the debug UI
  Mixer.h                 per peer decoding, jitter buffer, mixing, sounds
  Transport.h             packet format, ESP-NOW send and receive, reports, relay
  Source.h                timer driven sending, encoding, adaptation, slots, alerts
  Console.h               serial commands and JSON stats
AudioTools/, AudioTools.h vendored arduino-audio-tools (I2S output, volume, sine)
tools/debugui/            debug web UI (Python server and one HTML page)
tools/measure_response.py speaker frequency response via the Mac microphone
tools/monitor.sh          plain serial monitor for all connected boards
```

## Building and flashing

Requirements (macOS, Homebrew):

```sh
brew install arduino-cli ffmpeg
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32          # tested with 3.3.12
git clone https://github.com/pschatzmann/arduino-liblc3 ~/Documents/Arduino/libraries/arduino-liblc3
```

Build from the repository root. The optimisation flags matter: Arduino's default `-Os` makes LC3 noticeably slower.

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 --library . \
  --build-property "compiler.optimization_flags=-O2 -ffast-math" \
  --build-property "compiler.optimization_flags.release=-O2 -ffast-math" \
  espIntercom
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=460800 -p /dev/cu.usbserial-XXXX espIntercom
```

Upload at 460800 baud: the CH340 boards fail at 921600, both for flashing and for runtime serial on macOS.

The easiest way is the debug UI, which builds once and flashes every connected board in parallel.

## Debug UI

```sh
tools/debugui/run.sh       # creates its own venv on first run
open http://localhost:8765
```

The server owns the serial ports, so stop it before using `tools/monitor.sh` or the Arduino IDE.

- **Overview:** units heard live and a "who hears whom" matrix with loss and RSSI per link.
- **Per board tabs:** overview with sender table, radio charts, waveform and jitter buffer, settings, every raw stats field, log with a command field.
- **Build and flash** all boards or one.
- **Radio streaming** as a test source: the server decodes a stream with ffmpeg, paces it on the Mac clock and sends it to a board as mu law over USB.

## Serial commands

Commands are plain text lines at 460800 baud. Group settings (marked G) spread to the whole group.

| Command | Purpose |
|---|---|
| `tx on\|off`, `play on\|off` | Send audio / play received audio |
| `vol 0..100`, `tone <Hz>` | Volume, test tone frequency (0 means a per unit default) |
| `rate <Mbps>` (G) | PHY rate: 1, 2, 5.5, 11, 6, 9, 12, 18, 24 |
| `adapt on\|off` (G) | Adaptive bitrate |
| `kbps N`, `rkbps N`, `redundancy 0..2` (G) | Manual codec settings, used when adaptive is off |
| `eq on\|off`, `eqband <i> <Hz> <dB> <Q>`, `eqreset` | Voice EQ |
| `alerts on\|off` | Update needed and group full sounds |
| `chime startup\|status N\|join\|leave\|update\|full` | Preview a sound |
| `pins <bck> <ws> <data>` | I2S pins, saved, reboots |
| `reset`, `reboot` | Reset statistics, restart |
| `test on\|off` | Local test tone straight to the speaker |
| `src host\|sine` | Debug: audio source from the Mac or the sine |
| `simloss <%>`, `block <xx:yy>` | Test: simulated receive loss, ignore one sender's direct packets |
| `protover N`, `maxpeers N` | Test: fake protocol version, smaller peer table |
| `scope on\|off`, `cpustats on\|off`, `statsms N` | Debug output control |

## How it works

**Sending.** A hardware timer wakes the send task every 10 ms. It encodes one LC3 frame at the current bitrate plus a 16 kbps copy for redundancy. Every 20 ms it broadcasts a packet holding its two newest frames and the low bitrate copies of the previous two packets. Every 10th packet carries a reception report (how well this unit hears each other unit) and every 50th carries the group settings. Units with TX off send a small keepalive every 200 ms.

**Receiving.** Each sender gets its own LC3 decoder and jitter buffer. Lost packets are rebuilt from the redundancy in the next packets; longer gaps are filled by LC3's concealment. The buffer target follows the worst recent lateness of that sender, so clean links get low latency and lossy links more buffer. Clock drift between units is absorbed by stretching or squeezing a frame by one sample at a time.

**Slots.** Each unit measures when in the 20 ms cycle the others send. If another sender is closer than one packet airtime plus a margin, it moves its own timer into the middle of the widest free gap.

**Adaptation.** Each sender picks its profile from the worst listener's smoothed RSSI (with hysteresis), plus a collapse rule for loss above 10 % that persists for 1.5 s. Listeners served by a relay are judged by how they hear the relay.

**Relay.** A unit forwards a sender when a neighbour that hears this unit well reports hearing that sender badly or not at all. Among willing relays the lowest MAC keeps the job. Relayed copies go out right after the relay's own packet, inside its slot.

## Known limitations

- **No microphone yet.** The send side is a sine or Mac radio.
- **Debug code is compiled into every build.** It costs about 30 KB RAM, 1 to 2 % CPU and a once per second scheduler stall from the CPU measurement.
- **The Mac radio feed still loses a frame now and then**, because the radio driver stalls interrupts briefly about once a second. This only affects the debug feed over USB; the intercom audio path and the planned I2S microphone are not affected.
- **One hop relay only**, and switching to a relay takes about a second (one short dropout).
- **The channel is fixed** in firmware. Units on different firmware channels never find each other.
- **Up to 8 other units.** A newcomer only gets a slot from a unit that has been gone for more than 5 s.
- **No encryption.** Anyone with an ESP32 can listen in or inject packets.
- **Range not measured yet** beyond a closed door at a few metres.

## Next steps

In priority order:

1. **Microphone chain (INMP441).** I2S input sharing clocks with the amplifier, high pass against wind, AGC, noise gate or VAD (the keepalive mechanism for silent units already exists). Test mic placement, foam and a throat mic in real wind.
2. **Production build flag `INTERCOM_DEBUG`.** Compile out stats JSON, scope, pop detector, CPU measurement, host audio source, test knobs and UART tuning, so live units carry no debug cost.
3. **Range test.** 1 vs 6 Mbps and body shadowing outdoors, to settle the PHY rate and how eagerly the relay steps in.
4. **Amplifier mute when silent.** Wire the MAX98357A SD pin to a GPIO (for example GPIO21) to remove idle hiss and save power.
5. **Partition table and OTA.** The firmware uses 83 % of its slot. Switch to `min_spiffs` (1.9 MB per slot) with rollback, then OTA over WiFi in a maintenance mode.
6. **Better drivers.** Compare 40 mm 32 Ω helmet speakers and isolating earbuds (with a headphone DAC such as PCM5102A or WM8960) using `tools/measure_response.py`.
7. **Dynamic de-esser** if the static EQ is not enough for harsh S sounds.
8. **Hardware interface.** Volume and mute buttons, a status LED, battery monitoring on an ADC1 pin.
9. **Later.** Encryption, more than 8 units, faster relay takeover, multi hop relay, voice prompts, a gateway board for live monitoring of all units, automated tests of the jitter buffer and packet parsing on the computer.

### Skydiving features (later)

- **Altitude monitoring.** A barometric sensor (for example BMP390 or DPS310 on I2C) for altitude and freefall detection. Possible uses: audible altitude cues such as a breakoff tone, sharing each jumper's altitude with the group in the existing reception reports, and switching profiles on exit or deployment. Audible cues must stay a complement to a certified altimeter, never a replacement.
- **GPS.** A small GNSS module (for example u-blox M10) for position and speed: group positions on the ground station, landing pattern review, and data for the jump log.
- **Recording.** Store the jump's audio on the unit (SD card, or flash after the partition change). LC3 frames are already encoded, so recording them costs almost no CPU: about 4 KB/s per stream at 32 kbps, around 3.6 MB for 15 minutes. Recording each sender's stream separately allows replay and mixing afterwards. Combined with altitude, GPS and link statistics this becomes the black box: a full replay of each jump with audio, altitude, positions and radio quality. Writes need their own task so slow SD writes never stall audio.
