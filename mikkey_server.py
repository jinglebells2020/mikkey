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


def do_say(text: str, lid: str | None = None) -> dict:
    lid = lid or f"say-{int(time.time())}"
    ensure_clip(lid, text)
    with lock:
        queue.append(lid)
    return {"id": lid}


# ---------------- streaming TTS -> stick (low-latency live turns) ----------

REF_RMS = 14000.0     # fixed loudness reference (median max-RMS of real clips)
ATTACK, DECAY, FLOOR, SILENCE = 0.7, 0.25, 0.15, 0.02
CELL = 441            # samples per cell = one mouth frame
STREAM_RATE = 24000   # fish pcm supports 8k/16k/24k/32k/44.1k — not 22050


class CellSender:
    """Sends 20 ms envelope+PCM cells down an open MIKS stream."""

    def __init__(self, ser):
        self.ser = ser
        self.pending = b""
        self.env = 0.0
        self.cells = 0
        ser.write(b"MIKS" + struct.pack("<I", STREAM_RATE))

    def _send_cell(self, cell: bytes):
        import numpy as np
        raw = np.frombuffer(cell, dtype=np.int16).astype(np.float64)
        rms = min(np.sqrt((raw ** 2).mean()) / REF_RMS, 1.0)
        coef = ATTACK if rms > self.env else DECAY
        self.env += coef * (rms - self.env)
        val = max(self.env, FLOOR) if rms > SILENCE else self.env
        self.ser.write(b"D" + bytes([int(val * 255)]) + cell)
        self.cells += 1
        time.sleep(0.010)   # ~1.4x real-time: CDC RX ring floods above this

    def feed(self, pcm: bytes):
        self.pending += pcm
        while len(self.pending) >= CELL * 2:
            cell, self.pending = self.pending[:CELL * 2], self.pending[CELL * 2:]
            self._send_cell(cell)

    def silence(self):
        """One real-time silent cell — keeps the stream alive while waiting."""
        self.ser.write(b"D\x00" + b"\x00" * (CELL * 2))
        self.cells += 1
        time.sleep(441 / STREAM_RATE)

    def finish(self):
        if self.pending:
            self._send_cell(self.pending + b"\x00" * (CELL * 2 - len(self.pending)))
        self.ser.write(b"E")
        self.ser.flush()


# Small talk gets an instant pre-generated sung reply — no LLM, no Fish call.
# Matched against the whisper transcript (short utterances only).
QUICK_REPLIES = {
    "greeting": [
        "[sung, cheerful] Helloooo my friend! So happy to seeee you!",
        "[sung like a jingle] Hi hi hiii! Mikkey at your serviiice!",
        "[sung, warm] Heyyy there, wonderful humaaan!",
    ],
    "howareyou": [
        "[sung, upbeat] I'm feeling electric, thank you for asking, la la laaa!",
        "[sung, theatrical] Marvelous, magnificent, my circuits are siiinging!",
    ],
    "name": [
        "[sung like a jingle] I'm Mikkey, Mikkey, the buddy on your deeesk!",
    ],
    "thanks": [
        "[sung, warm] You're welcome, you're welcome, anytiiime my friend!",
    ],
    "bye": [
        "[sung, soft and sweet] Byeee byeee, come back soooon, I'll be riiight here!",
    ],
}


def match_quick(heard: str) -> str | None:
    import re
    t = re.sub(r"[^a-z' ]", "", heard.lower()).strip()
    words = t.split()
    if not words or len(words) > 6:      # long utterance = real question
        return None
    j = " ".join(words)
    if any(p in j for p in ("how are you", "how're you", "how you doing",
                            "how's it going", "hows it going", "what's up", "whats up")):
        return "howareyou"
    if any(p in j for p in ("your name", "who are you")):
        return "name"
    if "thank" in j:
        return "thanks"
    if "goodbye" in j or "bye" in words or "see you" in j:
        return "bye"
    if words[0] in ("hi", "hello", "hey", "yo", "hiya", "greetings", "sup"):
        return "greeting"
    return None


def pick_quick(intent: str):
    import random
    opts = sorted(CACHE.glob(f"quick-{intent}-*.pcm24"))
    if not opts:
        return None, None
    p = random.choice(opts)
    return p.read_bytes(), QUICK_REPLIES[intent][int(p.stem.split("-")[-1])]


# Thinking noises: pre-generated hums pushed the instant the button releases,
# so Mikkey is never silent while whisper/LLM/Fish do their work.
FILLERS = [
    "[humming thoughtfully, slow] Hmmmm... hm hm hmmmm...",
    "[sung softly, pondering] Oooh, hmmm, let me thiiiink...",
    "[playful curious thinking noises] Uhmmm... hmm hmm... oooh!",
    "[humming a cheerful little tune while thinking] Hm hm hmmm, la la hmmm...",
]
_filler_last = -1


def ensure_fillers():
    CACHE.mkdir(exist_ok=True)
    from fishaudio.types import TTSConfig
    todo = [(CACHE / f"filler-{i}.pcm24", t) for i, t in enumerate(FILLERS)]
    todo += [(CACHE / f"quick-{intent}-{i}.pcm24", t)
             for intent, texts in QUICK_REPLIES.items()
             for i, t in enumerate(texts)]
    for p, text in todo:
        if p.exists():
            continue
        print(f"[canned] generating {p.stem}: {text[:50]}")
        pcm = client.tts.convert(text=text, model=MODEL, format="pcm",
                                 reference_id=REF_ID,
                                 config=TTSConfig(format="pcm", sample_rate=STREAM_RATE))
        p.write_bytes(pcm)
    print("[canned] fillers + quick replies ready")


def pick_filler() -> bytes:
    import random
    global _filler_last
    options = [i for i in range(len(FILLERS))
               if (CACHE / f"filler-{i}.pcm24").exists() and i != _filler_last]
    if not options:
        return b""
    _filler_last = random.choice(options)
    return (CACHE / f"filler-{_filler_last}.pcm24").read_bytes()


def stream_live(ser, filler_pcm: bytes, get_reply):
    """One continuous stream: thinking-hum filler -> silence padding while the
    brain/Fish work -> the sung answer. get_reply() blocks until the reply
    text is ready (or returns None)."""
    import queue as queue_mod
    q = queue_mod.Queue()
    lid = f"say-{int(time.time())}"

    def feeder():
        text = get_reply()
        if not text:
            q.put(None)
            return
        try:
            from fishaudio.types import TTSConfig
            t0 = time.time()
            first = True
            for chunk in client.tts.stream(
                    text=text, model=MODEL, format="pcm", reference_id=REF_ID,
                    config=TTSConfig(format="pcm", sample_rate=STREAM_RATE)):
                if first:
                    print(f"[stream] first fish audio after {time.time()-t0:.2f}s")
                    first = False
                q.put(chunk)
        except Exception as e:
            print(f"[err] fish stream died: {e}")
        q.put(None)

    threading.Thread(target=feeder, daemon=True).start()
    sender = CellSender(ser)
    if filler_pcm:
        sender.feed(filler_pcm)
    all_pcm, ended = [], False
    while not ended:
        try:
            chunk = q.get(timeout=441 / STREAM_RATE)
            if chunk is None:
                ended = True
            else:
                all_pcm.append(chunk)
                sender.feed(chunk)
        except queue_mod.Empty:
            sender.silence()    # Mikkey pauses, mouth closed, stream alive
    sender.finish()
    print(f"[stream] live turn: {sender.cells} cells "
          f"({sender.cells * CELL / STREAM_RATE:.1f}s incl. filler)")

    if all_pcm:
        import wave
        OUT.mkdir(exist_ok=True)
        with wave.open(str(OUT / f"{lid}.wav"), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(STREAM_RATE)
            w.writeframes(b"".join(all_pcm))


def stream_say(ser, text: str, lid: str | None = None):
    """Stream Fish Audio PCM straight to the stick while it generates.
    Playback starts ~0.5 s in. Archives the audio as out/{lid}.wav."""
    import numpy as np
    lid = lid or f"say-{int(time.time())}"
    print(f"[stream] fish-audio: {lid!r}: {text[:60]}...")
    t0 = time.time()
    from fishaudio.types import TTSConfig
    stream = client.tts.stream(
        text=text, model=MODEL, format="pcm", reference_id=REF_ID,
        config=TTSConfig(format="pcm", sample_rate=STREAM_RATE))

    ser.write(b"MIKS" + struct.pack("<I", STREAM_RATE))
    pending = b""
    all_pcm = []
    env_state, first = 0.0, True
    cells = 0
    try:
        for chunk in stream:
            if first:
                print(f"[stream] first audio after {time.time()-t0:.2f}s")
                first = False
            all_pcm.append(chunk)
            pending += chunk
            while len(pending) >= CELL * 2:
                cell, pending = pending[:CELL * 2], pending[CELL * 2:]
                raw = np.frombuffer(cell, dtype=np.int16).astype(np.float64)
                rms = min(np.sqrt((raw ** 2).mean()) / REF_RMS, 1.0)
                coef = ATTACK if rms > env_state else DECAY
                env_state += coef * (rms - env_state)
                val = max(env_state, FLOOR) if rms > SILENCE else env_state
                ser.write(b"D" + bytes([int(val * 255)]) + cell)
                cells += 1
                # ~1.4x real-time: fast enough that the buffer only grows,
                # slow enough that the stick's tiny CDC RX ring never floods
                time.sleep(0.010)
    except Exception as e:
        print(f"[err] fish stream died: {e} — ending clip early")
    if pending:
        cell = pending + b"\x00" * (CELL * 2 - len(pending))
        ser.write(b"D" + bytes([int(FLOOR * 255)]) + cell)
        cells += 1
    ser.write(b"E")
    ser.flush()
    secs = cells * CELL / STREAM_RATE
    print(f"[stream] {lid}: {cells} cells ({secs:.1f}s) sent in {time.time()-t0:.2f}s")

    pcm = b"".join(all_pcm)
    import wave
    OUT.mkdir(exist_ok=True)
    with wave.open(str(OUT / f"{lid}.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(STREAM_RATE)
        w.writeframes(pcm)
    return lid


# ---------------- stick-mic push-to-talk (whisper + Claude) ----------------

_talk = {"whisper": None, "brain": None, "history": [], "loaded": False}


def handle_mic_audio(pcm16: bytes, ser=None):
    """Raw s16le 16 kHz from the stick's mic -> transcribe -> reply -> sing.
    With ser: streams the reply live (low latency). Without: queued path."""
    import numpy as np
    from mikkey_talk import load_whisper, transcribe, make_brain, think

    if not _talk["loaded"]:
        _talk["whisper"] = load_whisper()
        _talk["brain"] = make_brain()
        _talk["loaded"] = True
    audio = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32) / 32768.0
    secs = len(audio) / 16000
    if secs < 0.5:
        print(f"[mic] {secs:.1f}s — too short, ignored")
        return

    # transcription is fast (~0.4s) — do it first, then pick a path
    heard = transcribe(_talk["whisper"], audio)
    if not heard:
        print("[mic] heard nothing")
        return

    # fast path: small talk gets an instant cached sung reply
    intent = match_quick(heard)
    if intent and ser is not None:
        pcm, text = pick_quick(intent)
        if pcm:
            print(f"[quick] {intent}: {text}")
            _talk["history"] += [{"role": "user", "content": heard},
                                 {"role": "assistant", "content": text}]
            sender = CellSender(ser)
            sender.feed(pcm)
            sender.finish()
            return

    # slow path: LLM in the background while the thinking hum plays
    result = {}
    def brain_work():
        try:
            result["reply"] = think(_talk["brain"], _talk["history"], heard)
            print(f"[mikkey] {result['reply']}")
        except Exception as e:
            print(f"[err] brain failed: {e}")
    th = threading.Thread(target=brain_work, daemon=True)
    th.start()

    if ser is None:
        th.join()
        if result.get("reply"):
            do_say(result["reply"])
        return

    def get_reply():
        th.join()
        return result.get("reply")
    try:
        stream_live(ser, pick_filler(), get_reply)
    except Exception as e:
        print(f"[err] live stream failed ({e}), falling back to queued path")
        th.join()
        if result.get("reply"):
            do_say(result["reply"])


# ---------------- USB serial link to the StickS3 ----------------

def clip_blob(lid: str) -> bytes:
    pcm = (CACHE / f"{lid}.pcm").read_bytes()
    env = (CACHE / f"{lid}.env").read_bytes()
    return MAGIC + struct.pack("<III", SAMPLE_RATE, len(pcm) // 2, len(env)) + env + pcm


def serial_worker(port: str):
    stick_busy = False
    busy_until = 0.0
    recording = False
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
                    elif line == "REC":
                        recording = True   # hold pushes while the mic is live
                    elif line.startswith("MIC "):
                        recording = False
                        try:
                            n = int(line.split()[1])
                            want = n * 2
                            buf = bytearray()
                            deadline = time.time() + 10
                            while len(buf) < want and time.time() < deadline:
                                chunk = ser.read(want - len(buf))
                                if chunk:
                                    buf.extend(chunk)
                            print(f"[mic] got {len(buf)}/{want} bytes "
                                  f"({n/16000:.1f}s of speech)")
                            if len(buf) == want:
                                handle_mic_audio(bytes(buf), ser)
                            else:
                                print("[err] mic transfer incomplete, dropped")
                        except Exception as e:
                            print(f"[err] mic handling failed: {e}")
                    elif line.startswith(("DONE", "READY")):
                        stick_busy = False
                if stick_busy and time.time() > busy_until:
                    print("[usb] busy timeout, assuming stick is idle")
                    stick_busy = False
                if not stick_busy and not recording:
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
            try:
                self.send_json(do_say(text, body.get("id")))
            except Exception as e:
                print(f"[err] generation failed: {e}")
                self.send_json({"error": str(e)}, 500)

        elif self.path == "/trigger":
            try:
                self.send_json(do_trigger())
            except Exception as e:
                print(f"[err] generation failed: {e}")
                self.send_json({"error": str(e)}, 500)
        else:
            self.send_json({"error": "not found"}, 404)


def main():
    threading.Thread(target=ensure_fillers, daemon=True).start()
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
