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


def normalize_pcm(data: bytes) -> bytes:
    """Scale s16le PCM so its loudest 20ms window sits at REF_RMS.
    Kills the ~12dB loudness spread between generated clips."""
    import numpy as np
    s = np.frombuffer(data, dtype=np.int16).astype(np.float64)
    n = len(s) // CELL
    if n == 0:
        return data
    rms = np.sqrt((s[: n * CELL].reshape(n, CELL) ** 2).mean(axis=1))
    peak = rms.max()
    if peak < 1:
        return data
    gain = min(max(REF_RMS / peak, 0.25), 4.0)
    return np.clip(s * gain, -32767, 32767).astype(np.int16).tobytes()


def ensure_clip(lid: str, text: str) -> None:
    """Generate mp3 (unless cached) and normalized pcm+env (unless cached)."""
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
        pcm.write_bytes(normalize_pcm(pcm.read_bytes()))
        env.write_bytes(compute_envelope(pcm).tobytes())
        print(f"[gen] {lid}: pcm+env cached (normalized)")


def migrate_cache():
    """One-time loudness normalization of clips cached before normalization
    existed. The out/*.mp3 originals are never touched."""
    marker = CACHE / ".normalized"
    if marker.exists() or not CACHE.exists():
        return
    for p in sorted(CACHE.glob("*.pcm")) + sorted(CACHE.glob("*.pcm24")):
        p.write_bytes(normalize_pcm(p.read_bytes()))
        if p.suffix == ".pcm":
            (CACHE / f"{p.stem}.env").write_bytes(compute_envelope(p).tobytes())
    marker.write_text("v1")
    print("[migrate] cache loudness normalized")


def do_trigger() -> dict:
    """Generate the current script line, queue it, then advance — a failed
    generation must NOT skip the line for the whole shoot."""
    global script_pos
    with lock:
        pos = script_pos
        line = SCRIPT_LINES[pos % len(SCRIPT_LINES)]
    print(f"[trigger] script line {pos + 1}: {line['id']}")
    ensure_clip(line["id"], line["text"])   # raises on failure, pos unchanged
    with lock:
        script_pos = pos + 1
        queue.append(line["id"])
    return {"id": line["id"], "pos": pos + 1}


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


def mood_of(text: str) -> int:
    """0 plain, 1 upbeat, 2 sad, 3 sleepy — from the reply's style tag."""
    t = text.lower()
    if any(w in t for w in ("sad", "ballad", "mournful", "melanchol", "sorrow",
                            "apolog", "sheepish", "tearful")):
        return 2
    if any(w in t for w in ("lullaby", "sleepy", "drowsy", "soft and slow", "whisper")):
        return 3
    if any(w in t for w in ("upbeat", "cheerful", "jingle", "rock", "anthem",
                            "excited", "theatrical", "energetic", "happy",
                            "playful", "jazzy", "triumphant")):
        return 1
    return 0


class CellSender:
    """Sends 20 ms envelope+PCM cells down an open MIKS stream."""

    def __init__(self, ser):
        self.ser = ser
        self.pending = b""
        self.env = 0.0
        self.cells = 0
        ser.write(b"MIKS" + struct.pack("<I", STREAM_RATE))

    def set_mood(self, mood: int):
        """In-stream mood tag — colors the stick's singing + flourish."""
        self.ser.write(b"M" + bytes([mood & 3]))

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

    def pad_to_cell(self):
        """Flush a partial cell now (zero-padded) so e.g. the filler's tail
        can't splice onto the front of the answer."""
        if self.pending:
            self._send_cell(self.pending + b"\x00" * (CELL * 2 - len(self.pending)))
            self.pending = b""

    def finish(self):
        self.pad_to_cell()
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
    texts = QUICK_REPLIES[intent]
    try:
        idx = min(int(p.stem.split("-")[-1]), len(texts) - 1)
    except ValueError:
        idx = 0
    return p.read_bytes(), texts[idx]


# Thinking noises: pre-generated hums pushed the instant the button releases,
# so Mikkey is never silent while whisper/LLM/Fish do their work.
FILLERS = [
    "[humming thoughtfully, slow] Hmmmm... hm hm hmmmm...",
    "[sung softly, pondering] Oooh, hmmm, let me thiiiink...",
    "[playful curious thinking noises] Uhmmm... hmm hmm... oooh!",
    "[humming a cheerful little tune while thinking] Hm hm hmmm, la la hmmm...",
]
_filler_last = -1

# Streamed to the stick when the brain or Fish dies mid-turn, so failure is a
# charming sung apology instead of silence on camera.
ERROR_LINES = [
    "[sung sadly, apologetic] Oh nooo, my brain got stuuuck! Ask me agaaain?",
    "[sung, sheepish and cute] Oopsie doopsie, my thoughts fell over! One more tiiime?",
]


def ensure_fillers():
    """Pre-generate every canned clip, with retry — Fish being down at launch
    must never silently strip Mikkey of his fillers and error lines."""
    CACHE.mkdir(exist_ok=True)
    from fishaudio.types import TTSConfig
    todo = [(CACHE / f"filler-{i}.pcm24", t) for i, t in enumerate(FILLERS)]
    todo += [(CACHE / f"quick-{intent}-{i}.pcm24", t)
             for intent, texts in QUICK_REPLIES.items()
             for i, t in enumerate(texts)]
    todo += [(CACHE / f"error-{i}.pcm24", t) for i, t in enumerate(ERROR_LINES)]
    for attempt in range(5):
        missing = [(p, t) for p, t in todo if not p.exists()]
        if not missing:
            break
        for p, text in missing:
            try:
                print(f"[canned] generating {p.stem}: {text[:50]}")
                pcm = client.tts.convert(
                    text=text, model=MODEL, format="pcm", reference_id=REF_ID,
                    config=TTSConfig(format="pcm", sample_rate=STREAM_RATE))
                p.write_bytes(normalize_pcm(pcm))
            except Exception as e:
                print(f"[err] canned gen failed for {p.stem}: {e}")
        if any(not p.exists() for p, _ in todo):
            print(f"[canned] retrying missing clips in {15 * (attempt + 1)}s")
            time.sleep(15 * (attempt + 1))
    missing = [p.stem for p, _ in todo if not p.exists()]
    if missing:
        print(f"[err] STILL MISSING canned clips after retries: {missing}")
    else:
        print("[canned] fillers + quick replies + error lines ready")


def pick_error_clip() -> bytes:
    import random
    opts = sorted(CACHE.glob("error-*.pcm24"))
    return random.choice(opts).read_bytes() if opts else b""


def ensure_script_clips():
    """Pre-generate every script.yaml line at startup so a /trigger during
    filming is always instant, with retry."""
    for attempt in range(5):
        failed = []
        for line in SCRIPT_LINES:
            try:
                ensure_clip(line["id"], line["text"])
            except Exception as e:
                failed.append(line["id"])
                print(f"[err] script pre-gen failed for {line['id']}: {e}")
        if not failed:
            print(f"[script] all {len(SCRIPT_LINES)} lines cached and ready")
            return
        print(f"[script] retrying {failed} in {15 * (attempt + 1)}s")
        time.sleep(15 * (attempt + 1))
    print("[err] script lines STILL missing after retries — /trigger will generate live")


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
        q.put(("mood", mood_of(text)))
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
        sender.pad_to_cell()   # never splice the filler's tail onto the answer
    all_pcm, ended = [], False
    while not ended:
        try:
            chunk = q.get(timeout=441 / STREAM_RATE)
            if chunk is None:
                ended = True
            elif isinstance(chunk, tuple) and chunk[0] == "mood":
                sender.set_mood(chunk[1])
            else:
                all_pcm.append(chunk)
                sender.feed(chunk)
        except queue_mod.Empty:
            sender.silence()    # Mikkey pauses, mouth closed, stream alive
    if not all_pcm:
        # brain or Fish produced nothing — sing the apology, never go silent
        print("[stream] no answer audio — playing error clip")
        err = pick_error_clip()
        if err:
            sender.set_mood(2)   # sad
            sender.feed(err)
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
_talk_lock = threading.Lock()


def preload_talk():
    """Load whisper + brain at startup so the first mic turn isn't 10s of
    dead robot on camera."""
    from mikkey_talk import load_whisper, make_brain
    with _talk_lock:
        if not _talk["loaded"]:
            _talk["whisper"] = load_whisper()
            _talk["brain"] = make_brain()
            _talk["loaded"] = True
    print("[preload] whisper + brain warm")


def handle_mic_audio(pcm16: bytes, ser=None):
    """Raw s16le 16 kHz from the stick's mic -> transcribe -> reply -> sing.
    With ser: streams the reply live (low latency). Without: queued path."""
    import numpy as np
    from mikkey_talk import load_whisper, transcribe, make_brain, think

    with _talk_lock:
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
            sender.set_mood(mood_of(text))
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


# ---------------- links to the StickS3: USB serial and TCP -----------------
#
# The device is the single authority over which link is live: it prints
# "POLL ..." on the link it is actually reading whenever it is idle, and the
# server only ever pushes a clip in response to a POLL on that same
# connection. No POLL, no push — the dual-link race class is dead.

TCP_PORT = 8091


class SockConn:
    """Duck-types the pyserial interface over a TCP socket. Non-blocking with
    select: a socket-level timeout would also apply to sendall(), and a
    timed-out sendall may have PARTIALLY sent — instant protocol desync."""

    def __init__(self, sock):
        import select
        self.select = select
        self.s = sock
        self.s.setblocking(False)
        self.buf = b""

    def _readable(self, timeout):
        r, _, _ = self.select.select([self.s], [], [], timeout)
        return bool(r)

    @property
    def in_waiting(self):
        return len(self.buf) + (4096 if self._readable(0) else 0)

    def _recv_into_buf(self):
        data = self.s.recv(65536)
        if data == b"":
            raise OSError("connection closed")
        self.buf += data

    def read(self, n):
        if not self.buf and self._readable(0.05):
            self._recv_into_buf()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def readline(self):
        deadline = time.time() + 0.05
        while b"\n" not in self.buf:
            if not self._readable(max(0.0, deadline - time.time())):
                return b""
            self._recv_into_buf()
        line, _, self.buf = self.buf.partition(b"\n")
        return line + b"\n"

    def write(self, data):
        total, t0, warned = 0, time.time(), False
        while total < len(data):
            _, w, _ = self.select.select([], [self.s], [], 0.25)
            if w:
                total += self.s.send(data[total:])
                continue
            waited = time.time() - t0
            if waited > 1.0 and not warned:
                warned = True
                print(f"[tcp] backpressure {waited*1000:.0f}ms")
            if waited > 5.0:
                raise OSError(f"tcp write timeout with {len(data)-total}B unsent")
        if warned:
            print(f"[tcp] backpressure cleared after {(time.time()-t0)*1000:.0f}ms")

    def flush(self):
        pass


def tcp_listener():
    import socket
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", TCP_PORT))
    srv.listen(1)
    print(f"[tcp] listening on 0.0.0.0:{TCP_PORT}")
    while True:
        sock, addr = srv.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"[tcp] stick connected from {addr[0]}")
        try:
            link_worker(SockConn(sock), "tcp", heartbeat=True)
        except Exception as e:
            print(f"[tcp] link lost: {e}")
        finally:
            try:
                sock.close()
            except OSError:
                pass
            print("[tcp] waiting for stick to reconnect")


def clip_blob(lid: str) -> bytes:
    pcm = (CACHE / f"{lid}.pcm").read_bytes()
    env = (CACHE / f"{lid}.env").read_bytes()
    return MAGIC + struct.pack("<III", SAMPLE_RATE, len(pcm) // 2, len(env)) + env + pcm


def link_worker(conn, name: str, heartbeat: bool):
    """Drives one stick link (pyserial Serial or SockConn) until it dies.
    Clips are pushed ONLY in response to a POLL received on this connection,
    so a push can never go down a link the device isn't reading."""
    stick_busy = False
    busy_lid = None
    got_play = False
    busy_until = 0.0
    pushed_at = 0.0
    recording = False
    last_seen = time.time()

    def requeue(reason):
        nonlocal stick_busy, busy_lid, got_play
        if busy_lid and not got_play:
            print(f"[{name}] {reason} — requeueing {busy_lid} at front")
            with lock:
                queue.insert(0, busy_lid)
        elif busy_lid:
            print(f"[{name}] {reason} — {busy_lid} had started playing, not requeued")
        stick_busy = False
        busy_lid = None
        got_play = False

    while True:
        raw = conn.readline()
        if raw:
            last_seen = time.time()
            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            if not line.startswith("POLL"):
                print(f"[stick/{name}] {line}")
            if line == "TRIG":
                try:
                    do_trigger()
                except Exception as e:
                    print(f"[err] trigger failed: {e}")
            elif line == "REC":
                recording = True
            elif line.startswith("MIC "):
                recording = False
                try:
                    n = int(line.split()[1])
                    want = n * 2
                    buf = bytearray()
                    deadline = time.time() + 10
                    while len(buf) < want and time.time() < deadline:
                        chunk = conn.read(want - len(buf))
                        if chunk:
                            buf.extend(chunk)
                    print(f"[mic] got {len(buf)}/{want} bytes "
                          f"({n/16000:.1f}s of speech)")
                    if len(buf) == want:
                        handle_mic_audio(bytes(buf), conn)
                    else:
                        print("[err] mic transfer incomplete, dropped")
                    # discard lines that piled up while we streamed the reply:
                    # stale POLL/PLAY/DONE here caused pushes mid-playback and
                    # mis-attributed busy state (heartbeat killed a live link)
                    stale = 0
                    while conn.readline():
                        stale += 1
                    if stale:
                        print(f"[{name}] discarded {stale} stale lines after mic turn")
                    last_seen = time.time()
                except (OSError, pyserial.SerialException):
                    raise
                except Exception as e:
                    print(f"[err] mic handling failed: {e}")
            elif line.startswith("PLAY"):
                if stick_busy:
                    got_play = True
            elif line.startswith(("DONE", "READY")):
                stick_busy = False
                busy_lid = None
                got_play = False
            elif line.startswith("POLL"):
                recording = False
                if stick_busy and time.time() - pushed_at > 3:
                    requeue("device polls but push was never acked")
                if not stick_busy:
                    with lock:
                        lid = queue.pop(0) if queue else None
                    if lid:
                        blob = clip_blob(lid)
                        secs = len(blob) / (2 * SAMPLE_RATE)
                        print(f"[{name}] pushing {lid}: {len(blob)} bytes (~{secs:.1f}s)")
                        stick_busy = True
                        busy_lid = lid
                        got_play = False
                        pushed_at = time.time()
                        busy_until = pushed_at + secs + 10
                        # 1KB slices, 4ms apart (~250 kB/s): the S3's USB-serial
                        # hardware stalls on large sustained writes. Harmless on TCP.
                        for i in range(0, len(blob), 1024):
                            conn.write(blob[i:i + 1024])
                            conn.flush()
                            time.sleep(0.004)
                        conn.flush()
                        print(f"[{name}] pushed in {time.time()-pushed_at:.2f}s")
        if stick_busy and time.time() > busy_until:
            requeue("busy timeout")
        # heartbeat (TCP only): device POLLs every 500ms when idle, and any
        # line counts as life. Silence while idle => half-open socket, kill it.
        # Never fires while busy/recording — the device is rightly quiet then.
        if heartbeat and not stick_busy and not recording \
                and time.time() - last_seen > 6:
            raise OSError("heartbeat lost (no POLL for 6s while idle)")


def serial_worker():
    while True:
        port = find_stick_port()    # re-glob: the path changes across replugs
        if not port:
            time.sleep(3)
            continue
        try:
            ser = pyserial.Serial(port, 115200, timeout=0.05, write_timeout=2)
        except Exception as e:
            print(f"[usb] can't open {port}: {e} — retrying in 3s")
            time.sleep(3)
            continue
        print(f"[usb] connected {port}")
        try:
            link_worker(ser, "usb", heartbeat=False)
        except Exception as e:
            print(f"[usb] link lost: {e} — reconnecting")
            try:
                ser.close()
            except Exception:
                pass
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
    migrate_cache()
    threading.Thread(target=ensure_fillers, daemon=True).start()
    threading.Thread(target=ensure_script_clips, daemon=True).start()
    threading.Thread(target=preload_talk, daemon=True).start()
    threading.Thread(target=tcp_listener, daemon=True).start()
    threading.Thread(target=serial_worker, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"mikkey server on 0.0.0.0:{PORT}  "
          f"({len(SCRIPT_LINES)} script lines, model {MODEL})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
