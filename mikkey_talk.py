#!/usr/bin/env python3
"""Mikkey Task 5: push-to-talk mic loop on the laptop.

Enter = start recording, Enter again = stop. Speech -> whisper -> Claude (in
Mikkey's character) -> POST /say on the local server -> the stick sings it.

Needs mikkey_server.py running. Without ANTHROPIC_API_KEY in .env it falls
back to canned replies so the loop still demos.

Run:  .venv/bin/python mikkey_talk.py
"""

import json
import os
import sys
import urllib.request
from pathlib import Path

import numpy as np
import sounddevice as sd
from dotenv import load_dotenv

ROOT = Path(__file__).parent
load_dotenv(ROOT / ".env")

SERVER = "http://localhost:8090"
MIC_RATE = 16000
MIKKEY_STYLE = "[sung, theatrical]"  # default style tag; tweak after style verdict

SYSTEM = f"""You are Mikkey, a tiny singing robot buddy who lives on a desk.
You are cheerful, a little dramatic, and you sing EVERYTHING instead of speaking.

Reply with exactly ONE short line (max ~20 words) that will be sung out loud
by a TTS voice. Start the line with a style tag in square brackets, like
{MIKKEY_STYLE} or [sung like a sad ballad] — pick whichever fits the mood of
your reply. No quotes, no explanations, just the tag and the line."""

CANNED = [
    f"{MIKKEY_STYLE} Oh what a wonderful question, my friend, but my brain is offliiine!",
    "[sung like a sad ballad] I heard your words... but my API key... is missiiiing...",
    f"{MIKKEY_STYLE} La la laaa, I would love to answer, if only I could thiiink!",
]
canned_idx = 0


def record() -> np.ndarray:
    input("\n[mic] press ENTER to start recording...")
    print("[mic] recording — press ENTER to stop")
    chunks = []
    stream = sd.InputStream(samplerate=MIC_RATE, channels=1, dtype="float32",
                            callback=lambda data, *_: chunks.append(data.copy()))
    stream.start()
    input()
    stream.stop(); stream.close()
    audio = np.concatenate(chunks)[:, 0] if chunks else np.zeros(0, dtype="float32")
    print(f"[mic] captured {len(audio)/MIC_RATE:.1f}s")
    return audio


def load_whisper():
    from faster_whisper import WhisperModel
    print("[stt] loading whisper (first run downloads the model)...")
    model = WhisperModel("base", device="cpu", compute_type="int8")
    print("[stt] ready")
    return model


def transcribe(model, audio: np.ndarray) -> str:
    segments, _info = model.transcribe(audio, language="en")
    text = " ".join(s.text.strip() for s in segments).strip()
    print(f"[stt] heard: {text!r}")
    return text


def make_brain():
    if os.environ.get("OPENROUTER_API_KEY"):
        model = os.environ.get("OPENROUTER_MODEL", "deepseek/deepseek-v4-flash-latest")
        print(f"[brain] openrouter: {model}")
        return {"kind": "openrouter",
                "key": os.environ["OPENROUTER_API_KEY"], "model": model}
    if os.environ.get("ANTHROPIC_API_KEY"):
        import anthropic
        print("[brain] anthropic: claude-opus-5")
        return {"kind": "anthropic", "client": anthropic.Anthropic()}
    print("[brain] no OPENROUTER_API_KEY / ANTHROPIC_API_KEY in .env — canned replies")
    return None


def think(brain, history: list, heard: str) -> str:
    global canned_idx
    if brain is None:
        reply = CANNED[canned_idx % len(CANNED)]
        canned_idx += 1
        return reply
    history.append({"role": "user", "content": heard})
    if brain["kind"] == "openrouter":
        req = urllib.request.Request(
            "https://openrouter.ai/api/v1/chat/completions", method="POST",
            data=json.dumps({
                "model": brain["model"],
                "max_tokens": 300,
                "reasoning": {"effort": "low"},   # latency: don't let it ponder
                "messages": [{"role": "system", "content": SYSTEM}] + history[-10:],
            }).encode(),
            headers={"Authorization": f"Bearer {brain['key']}",
                     "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=60) as r:
            reply = json.loads(r.read())["choices"][0]["message"]["content"].strip()
    else:
        response = brain["client"].messages.create(
            model="claude-opus-5",
            max_tokens=300,
            system=SYSTEM,
            output_config={"effort": "low"},
            messages=history[-10:],
        )
        reply = next((b.text for b in response.content if b.type == "text"), "").strip()
    history.append({"role": "assistant", "content": reply})
    return reply


def say(text: str):
    req = urllib.request.Request(
        f"{SERVER}/say", method="POST",
        data=json.dumps({"text": text}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        print(f"[say] queued: {json.loads(r.read())}")


def main():
    try:
        with urllib.request.urlopen(f"{SERVER}/", timeout=3) as r:
            print(f"[server] {r.read().decode().strip()}")
    except Exception:
        sys.exit("mikkey_server.py is not running — start it first")

    whisper = load_whisper()
    brain = make_brain()
    history = []
    print("\n=== Mikkey push-to-talk. Ctrl+C to quit. ===")
    while True:
        audio = record()
        if len(audio) < MIC_RATE // 2:
            print("[mic] too short, try again")
            continue
        heard = transcribe(whisper, audio)
        if not heard:
            print("[stt] heard nothing, try again")
            continue
        reply = think(brain, history, heard)
        print(f"[mikkey] {reply}")
        try:
            say(reply)
        except Exception as e:
            print(f"[err] /say failed: {e}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")
