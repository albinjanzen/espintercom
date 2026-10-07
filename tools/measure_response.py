#!/usr/bin/env python3
import argparse
import array
import json
import math
import subprocess
import tempfile
import time
import urllib.request
import wave

FREQS = [200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300]
RECORD_RATE = 16000
WINDOW = 4000
HOP = 2000


def send(port, command):
    body = json.dumps({"port": port, "cmd": command}).encode()
    request = urllib.request.Request("http://localhost:8765/api/cmd", data=body, method="POST")
    urllib.request.urlopen(request).read()


def goertzel_power(samples, freq):
    k = 2 * math.cos(2 * math.pi * freq / RECORD_RATE)
    s1 = s2 = 0.0
    for v in samples:
        s1, s2 = v + k * s1 - s2, s1
    return (s1 * s1 + s2 * s2 - k * s1 * s2) / (len(samples) ** 2)


def measure(port, eq, volume):
    path = tempfile.mktemp(suffix=".wav")
    recorder = subprocess.Popen([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "avfoundation", "-i", ":0",
        "-ac", "1", "-ar", str(RECORD_RATE), "-t", str(len(FREQS) + 4), "-y", path,
    ])
    time.sleep(1.5)
    send(port, f"vol {volume}")
    send(port, f"eq {eq}")
    send(port, "test on")
    for freq in FREQS:
        send(port, f"tone {freq}")
        time.sleep(1.0)
    send(port, "test off")
    send(port, "tone 0")
    recorder.wait()

    with wave.open(path) as w:
        samples = array.array("h", w.readframes(w.getnframes()))
    # the Mac mic drops audio blocks, so take each tone's loudest window instead of trusting timing
    levels = {}
    for freq in FREQS:
        best = max(goertzel_power(samples[i:i + WINDOW], freq) for i in range(0, len(samples) - WINDOW, HOP))
        levels[freq] = 10 * math.log10(best + 1e-9)
    return levels


def main():
    parser = argparse.ArgumentParser(description="Play test tones on a board and measure them with the Mac mic (speaker ~5 cm from it)")
    parser.add_argument("port")
    parser.add_argument("--eq", default="off", choices=["on", "off"])
    parser.add_argument("--volume", type=int, default=40)
    args = parser.parse_args()

    levels = measure(args.port, args.eq, args.volume)
    median = sorted(levels.values())[len(levels) // 2]
    print(f"EQ {args.eq}, level relative to median:")
    for freq, level in levels.items():
        rel = level - median
        print(f"{freq:5d} Hz  {rel:+6.1f} dB  " + "#" * max(0, int(rel + 20)))
    spread = max(levels.values()) - min(levels.values())
    print(f"spread {spread:.1f} dB")


if __name__ == "__main__":
    main()
