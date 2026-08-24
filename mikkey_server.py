#!/usr/bin/env python3
"""Mikkey Task 3: host server. Plain stdlib, threaded, loud diagnostics.

  POST /say         {"text": "...", "id": "optional-id"} -> generate + queue
  POST /trigger     advance to next line of script.yaml, generate if needed, queue
  GET  /pending     {"pending": bool, "id": str|null}
  GET  /clip/<id>   16-byte header | envelope bytes | raw s16le PCM
                    header: b"MIKY" + u32 sample_rate + u32 pcm_samples + u32 env_len (LE)
  GET  /            status text

Run:  .venv/bin/python mikkey_server.py         (port 8090)
"""

import glob
import json
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial as pyserial

import yaml
from dotenv import load_dotenv
from fishaudio import FishAudio

from audio_pipeline import mp3_to_pcm, compute_envelope, SAMPLE_RATE, OUT, CACHE

ROOT = Path(__file__).parent
PORT = 8090
MAGIC = b"MIKY"

load_dotenv(ROOT / ".env")
client = FishAudio()

lock = threading.Lock()
queue = []          # clip ids waiting for the ESP32
script_pos = 0

script = yaml.safe_load((ROOT / "script.yaml").read_text())
MODEL = script.get("model", "s2.1-pro-free")
REF_ID = script.get("reference_id") or None
SCRIPT_LINES = script["lines"]


def ensure_clip(lid: str, text: str) -> None:
    """Generate mp3 (unless cached) and pcm+env (unless cached)."""
    OUT.mkdir(exist_ok=True)
    CACHE.mkdir(exist_ok=True)
    mp3 = OUT / f"{lid}.mp3"
    if not mp3.exists():
        print(f"[gen] fish-audio: {lid!r}: {text[:60]}...")
        t0 = time.time()
        audio = client.tts.convert(text=text, model=MODEL, format="mp3",
                                   reference_id=REF_ID)
        mp3.write_bytes(audio)
        print(f"[gen] {lid}: {len(audio)} bytes in {time.time()-t0:.1f}s")
    pcm, env = CACHE / f"{lid}.pcm", CACHE / f"{lid}.env"
    if not pcm.exists() or not env.exists():
        mp3_to_pcm(mp3, pcm)
        env.write_bytes(compute_envelope(pcm).tobytes())
        print(f"[gen] {lid}: pcm+env cached")


def do_trigger() -> dict:
    """Advance the script one line, generate if needed, queue it."""
    global script_pos
    with lock:
        line = SCRIPT_LINES[script_pos % len(SCRIPT_LINES)]
        script_pos += 1
        pos = script_pos
    print(f"[trigger] script line {pos}: {line['id']}")
    ensure_clip(line["id"], line["text"])
    with lock:
        queue.append(line["id"])
    return {"id": line["id"], "pos": pos}


# ---------------- USB serial link to the StickS3 ----------------

def clip_blob(lid: str) -> bytes:
    pcm = (CACHE / f"{lid}.pcm").read_bytes()
    env = (CACHE / f"{lid}.env").read_bytes()
    return MAGIC + struct.pack("<III", SAMPLE_RATE, len(pcm) // 2, len(env)) + env + pcm


def serial_worker(port: str):
    stick_busy = False
    busy_until = 0.0
    while True:
        try:
            ser = pyserial.Serial(port, 115200, timeout=0.05)
        except Exception as e:
            print(f"[usb] can't open {port}: {e} — retrying in 3s")
            time.sleep(3)
            continue
        print(f"[usb] connected {port}")
        try:
            while True:
                raw = ser.readline()
                if raw:
                    line = raw.decode(errors="replace").strip()
                    if not line:
                        continue
                    print(f"[stick] {line}")
                    if line == "TRIG":
                        try:
                            do_trigger()
                        except Exception as e:
                            print(f"[err] trigger failed: {e}")
                    elif line.startswith(("DONE", "READY")):
                        stick_busy = False
                if stick_busy and time.time() > busy_until:
                    print("[usb] busy timeout, assuming stick is idle")
                    stick_busy = False
                if not stick_busy:
                    with lock:
                        lid = queue.pop(0) if queue else None
                    if lid:
                        blob = clip_blob(lid)
                        secs = len(blob) / (2 * SAMPLE_RATE)
                        print(f"[usb] pushing {lid}: {len(blob)} bytes (~{secs:.1f}s)")
                        stick_busy = True
                        busy_until = time.time() + secs + 10
                        t0 = time.time()
                        # 1KB slices, 4ms apart (~250 kB/s): the S3's USB-serial
                        # hardware stalls permanently on large sustained writes.
                        for i in range(0, len(blob), 1024):
                            ser.write(blob[i:i + 1024])
                            ser.flush()
                            time.sleep(0.004)
                            while ser.in_waiting:          # keep logs flowing mid-push
                                l = ser.readline().decode(errors="replace").strip()
                                if l:
                                    print(f"[stick] {l}")
                        ser.flush()
                        print(f"[usb] pushed in {time.time()-t0:.2f}s")
        except (OSError, pyserial.SerialException) as e:
            print(f"[usb] link lost: {e} — reconnecting")
            time.sleep(2)


def find_stick_port() -> str | None:
    ports = glob.glob("/dev/cu.usbmodem*")
    return ports[0] if ports else None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[http] {self.client_address[0]} {fmt % args}")

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_body_json(self):
        n = int(self.headers.get("Content-Length", 0))
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        global queue
        if self.path == "/pending":
            with lock:
                lid = queue[0] if queue else None
            self.send_json({"pending": lid is not None, "id": lid})

        elif self.path.startswith("/clip/"):
            lid = self.path.split("/clip/", 1)[1]
            pcm_p, env_p = CACHE / f"{lid}.pcm", CACHE / f"{lid}.env"
            if not pcm_p.exists() or not env_p.exists():
                self.send_json({"error": "no such clip"}, 404)
                return
            pcm, env = pcm_p.read_bytes(), env_p.read_bytes()
            header = MAGIC + struct.pack("<III", SAMPLE_RATE, len(pcm) // 2, len(env))
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(header) + len(env) + len(pcm)))
            self.end_headers()
            self.wfile.write(header)
            self.wfile.write(env)
            self.wfile.write(pcm)
            with lock:
                if lid in queue:
                    queue.remove(lid)
            print(f"[clip] served {lid}: {len(pcm)//2} samples, {len(env)} env frames")

        elif self.path == "/":
            with lock:
                txt = (f"mikkey server. script pos {script_pos}/{len(SCRIPT_LINES)}, "
                       f"queue {queue}\n")
            body = txt.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        global script_pos
        if self.path == "/say":
            try:
                body = self.read_body_json()
                text = body["text"]
            except Exception as e:
                self.send_json({"error": f"bad body: {e}"}, 400)
                return
            lid = body.get("id") or f"say-{int(time.time())}"
            try:
                ensure_clip(lid, text)
            except Exception as e:
                print(f"[err] generation failed: {e}")
                self.send_json({"error": str(e)}, 500)
                return
            with lock:
                queue.append(lid)
            self.send_json({"id": lid})

        elif self.path == "/trigger":
            try:
                self.send_json(do_trigger())
            except Exception as e:
                print(f"[err] generation failed: {e}")
                self.send_json({"error": str(e)}, 500)
        else:
            self.send_json({"error": "not found"}, 404)


def main():
    port = sys.argv[sys.argv.index("--port") + 1] if "--port" in sys.argv else find_stick_port()
    if port:
        threading.Thread(target=serial_worker, args=(port,), daemon=True).start()
    else:
        print("[usb] no /dev/cu.usbmodem* found — serial link disabled, HTTP only")
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"mikkey server on 0.0.0.0:{PORT}  "
          f"({len(SCRIPT_LINES)} script lines, model {MODEL})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
