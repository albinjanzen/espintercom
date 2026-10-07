#!/usr/bin/env python3
import glob
import audioop
import json
import queue
import struct
import subprocess
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[2]
SKETCH = ROOT / "espIntercom"
FQBN = "esp32:esp32:esp32"
UPLOAD_FQBN = FQBN + ":UploadSpeed=460800"
# Arduino's default -Os roughly halves LC3 codec speed, and the codec is the CPU budget
OPTIMIZE = ["--build-property", "compiler.optimization_flags=-O2 -ffast-math",
            "--build-property", "compiler.optimization_flags.release=-O2 -ffast-math"]
BAUD = 460800
STREAM_CHUNK_BYTES = 640
STREAM_CHUNK_SECONDS = STREAM_CHUNK_BYTES / 2 / 16000
STREAM_PREBUFFER_BYTES = 16000 * 2 * 3 // 10
STREAM_MAX_BYTES = 16000 * 2 * 30
PORT_PATTERNS = ["/dev/cu.usbserial-*", "/dev/cu.usbmodem*", "/dev/cu.wchusbserial*"]
HTTP_PORT = 8765
HISTORY_LINES = 300

clients = []
clients_lock = threading.Lock()
build_lock = threading.Lock()


def broadcast(event):
    event.setdefault("ts", time.time())
    data = json.dumps(event)
    with clients_lock:
        for q in clients:
            q.put(data)


class Board:
    def __init__(self, port):
        self.port = port
        self.history = deque(maxlen=HISTORY_LINES)
        self.serial = None
        self.paused = threading.Event()
        self.alive = True
        self.write_lock = threading.Lock()
        threading.Thread(target=self.run, daemon=True).start()

    def open(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = BAUD
        s.timeout = 0.5
        # keep both low so opening the port does not trigger the auto-reset circuit
        s.dtr = False
        s.rts = False
        s.open()
        return s

    def run(self):
        while self.alive:
            if self.paused.is_set():
                time.sleep(0.2)
                continue
            try:
                if self.serial is None:
                    self.serial = self.open()
                    broadcast({"type": "port", "port": self.port, "state": "open"})
                raw = self.serial.readline()
            except (serial.SerialException, OSError):
                self.close()
                if not Path(self.port).exists():
                    self.alive = False
                    broadcast({"type": "port", "port": self.port, "state": "gone"})
                    return
                time.sleep(1)
                continue
            if raw:
                self.handle_line(raw.decode(errors="replace").rstrip("\r\n"))

    def handle_line(self, line):
        parsed = None
        start = line.find("{")
        if start >= 0:
            try:
                parsed = json.loads(line[start:])
            except ValueError:
                pass
        entry = {"type": "line", "port": self.port, "line": line, "json": parsed, "ts": time.time()}
        self.history.append(entry)
        broadcast(entry)

    def close(self):
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:
                pass
            self.serial = None

    def send(self, cmd):
        with self.write_lock:
            if self.serial is None:
                return False
            self.serial.write((cmd.strip() + "\n").encode())
            return True

    # mu-law halves the bytes on the serial link, which leaves the board room for interrupt stalls
    def send_audio(self, pcm):
        ulaw = audioop.lin2ulaw(pcm, 2)
        frame = b"\xa5\x5b" + struct.pack("<H", len(ulaw)) + ulaw
        with self.write_lock:
            if self.serial is None:
                return False
            self.serial.write(frame)
            return True

    def reset_board(self):
        if self.serial is None:
            return
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.rts = False

    def flash(self):
        stop_stream(self.port)
        self.paused.set()
        self.close()
        broadcast({"type": "port", "port": self.port, "state": "flashing"})
        try:
            ok = run_logged(
                ["arduino-cli", "upload", "--fqbn", UPLOAD_FQBN, "-p", self.port, str(SKETCH)],
                self.port,
            )
        finally:
            self.paused.clear()
        broadcast({"type": "port", "port": self.port, "state": "flashed" if ok else "flash-failed"})
        return ok


boards = {}
boards_lock = threading.Lock()
streams = {}
streams_lock = threading.Lock()


def ffmpeg_command(url):
    # live streams cannot run ahead of real time, and -re would stop ffmpeg catching up after a network
    # hiccup; local files need it, or they would be read in one go
    pace = [] if url.startswith(("http://", "https://")) else ["-re"]
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", *pace, "-i", url,
            "-ac", "1", "-ar", "16000", "-f", "s16le", "-"]


def read_ffmpeg(proc, buffer, lock, done):
    while proc.poll() is None:
        chunk = proc.stdout.read1(STREAM_CHUNK_BYTES)
        if not chunk:
            break
        with lock:
            buffer.extend(chunk)
            # live HLS arrives a whole segment at a time; only a stream far ahead of real time is trimmed
            if len(buffer) > STREAM_MAX_BYTES:
                del buffer[: len(buffer) - STREAM_PREBUFFER_BYTES]
    done.set()


# radio streams arrive in segment-sized bursts; pacing them on the Mac's clock gives the board a
# steady feed, and the board only has to absorb the small drift between the two clocks
def stream_worker(board, proc):
    board.send("src host")
    broadcast({"type": "stream", "port": board.port, "state": "playing"})
    buffer, lock, done = bytearray(), threading.Lock(), threading.Event()
    threading.Thread(target=read_ffmpeg, args=(proc, buffer, lock, done), daemon=True).start()
    try:
        primed = False
        next_send = time.monotonic()
        sent_bytes, report_at, reprimes = 0, time.monotonic(), 0
        while not (done.is_set() and not buffer):
            with streams_lock:
                if streams.get(board.port) is not proc:
                    break
            with lock:
                level = len(buffer)
            if not primed:
                if level < STREAM_PREBUFFER_BYTES and not done.is_set():
                    time.sleep(0.01)
                    next_send = time.monotonic()
                    continue
                primed = True
            with lock:
                chunk = bytes(buffer[:STREAM_CHUNK_BYTES])
                del buffer[: len(chunk)]
            if len(chunk) < STREAM_CHUNK_BYTES:
                primed = False
                reprimes += 1
            if chunk:
                board.send_audio(chunk[: len(chunk) - len(chunk) % 2])
                sent_bytes += len(chunk)
            if time.monotonic() - report_at > 5:
                elapsed = time.monotonic() - report_at
                broadcast({"type": "build", "port": board.port,
                           "line": f"stream: sent {sent_bytes / elapsed:.0f} B/s, mac buffer {level} B, reprimes {reprimes}"})
                sent_bytes, report_at = 0, time.monotonic()
            next_send += STREAM_CHUNK_SECONDS
            delay = next_send - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            elif delay < -0.2:
                next_send = time.monotonic()
    finally:
        proc.kill()
        err = proc.stderr.read().decode(errors="replace").strip() if proc.stderr else ""
        if err:
            broadcast({"type": "build", "port": board.port, "line": "ffmpeg: " + err[-300:]})
        board.send("src sine")
        with streams_lock:
            if streams.get(board.port) is proc:
                del streams[board.port]
        broadcast({"type": "stream", "port": board.port, "state": "stopped"})


def start_stream(board, url):
    stop_stream(board.port)
    proc = subprocess.Popen(ffmpeg_command(url), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    with streams_lock:
        streams[board.port] = proc
    threading.Thread(target=stream_worker, args=(board, proc), daemon=True).start()


def stop_stream(port):
    with streams_lock:
        proc = streams.pop(port, None)
    if proc:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_logged(cmd, port):
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        line = line.rstrip()
        if line and "Writing at" not in line:
            broadcast({"type": "build", "port": port, "line": line})
    return proc.wait() == 0


def compile_sketch():
    broadcast({"type": "build", "port": None, "line": "compiling..."})
    ok = run_logged(["arduino-cli", "compile", "--fqbn", FQBN, "--library", str(ROOT), *OPTIMIZE, str(SKETCH)], None)
    broadcast({"type": "build", "port": None, "line": "compile ok" if ok else "compile FAILED"})
    return ok


def flash_ports(ports):
    with build_lock:
        if not compile_sketch():
            return
        threads = [threading.Thread(target=boards[p].flash) for p in ports if p in boards]
        for t in threads:
            t.start()
        for t in threads:
            t.join()


def scan_ports():
    while True:
        present = set()
        for pattern in PORT_PATTERNS:
            present.update(glob.glob(pattern))
        with boards_lock:
            for port in present:
                if port not in boards or not boards[port].alive:
                    boards[port] = Board(port)
                    broadcast({"type": "port", "port": port, "state": "found"})
            for port in list(boards):
                if not boards[port].alive:
                    del boards[port]
        time.sleep(2)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            body = (Path(__file__).parent / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self.stream_events()
        else:
            self.send_error(404)

    def stream_events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        q = queue.Queue()
        with boards_lock:
            backlog = [{"type": "port", "port": p, "state": "found"} for p in boards]
            backlog += [{"type": "stream", "port": p, "state": "playing"} for p in streams]
            for board in boards.values():
                backlog.extend(board.history)
        with clients_lock:
            clients.append(q)
        try:
            for event in backlog:
                self.wfile.write(f"data: {json.dumps(event)}\n\n".encode())
            self.wfile.flush()
            while True:
                try:
                    data = q.get(timeout=15)
                    self.wfile.write(f"data: {data}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with clients_lock:
                clients.remove(q)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        port = body.get("port")
        targets = list(boards.values()) if port == "all" else [boards[port]] if port in boards else []
        if not targets:
            return self.send_json({"error": "unknown port"}, 404)

        if self.path == "/api/cmd":
            sent = [b.port for b in targets if b.send(body.get("cmd", ""))]
            for p in sent:
                broadcast({"type": "sent", "port": p, "line": body.get("cmd", "")})
            return self.send_json({"sent": sent})
        if self.path == "/api/stream":
            for b in targets:
                if body.get("source") == "url":
                    start_stream(b, body.get("url", ""))
                else:
                    stop_stream(b.port)
            return self.send_json({"ok": True})
        if self.path == "/api/hwreset":
            for b in targets:
                b.reset_board()
            return self.send_json({"ok": True})
        if self.path == "/api/flash":
            if build_lock.locked():
                return self.send_json({"error": "build already running"}, 409)
            threading.Thread(target=flash_ports, args=([b.port for b in targets],), daemon=True).start()
            return self.send_json({"started": True})
        self.send_error(404)


if __name__ == "__main__":
    threading.Thread(target=scan_ports, daemon=True).start()
    print(f"Debug UI on http://localhost:{HTTP_PORT}")
    ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler).serve_forever()
