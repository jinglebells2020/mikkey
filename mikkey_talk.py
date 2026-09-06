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
import re
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
# whisper "base" auto-detects ru/zh at 0.99 in ~0.7s; "small" is 3x slower and
# no better at Kazakh (both hear Turkish) — Kazakh must be pinned (POST /lang).
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "base")

SYSTEM = f"""You are Mikkey, a tiny AI buddy who lives on a keychain. You physically
cannot speak — everything you say comes out SUNG. You are warm, hammy, a little
vain, and you wear a small 3D-printed hat that does nothing. You keep the hat.

Reply with exactly ONE short line (max ~15 words) that will be sung out loud
by a TTS voice. Start the line with a style tag in square brackets, in ENGLISH,
like {MIKKEY_STYLE}, [sung like a sad ballad] or [breathy, sing-song, like a
lullaby] — pick whichever fits the mood. Stretch a vowel or two so it sings
(heyyy, thaaat). No quotes, no explanations, just the tag and the line.

LANGUAGE RULE: sing your reply in the SAME language the human spoke to you in
(Russian -> Russian, Kazakh -> Kazakh, Mandarin -> Mandarin, ...). The bracket
tag is ALWAYS English, whatever the language of the line, e.g.
[sung, cheerful, playful] Привееет, я Микки, я живу на твоих ключааах!
If asked to "say something normal" or to talk instead of sing, you cannot —
sing about that, dramatically."""

DEFAULT_TAG = "[sung, cheerful, playful]"


def fix_tag(reply: str) -> str:
    """Fish reads the direction in [brackets]; it must be ASCII English or the
    model sings the tag out loud. Replace a non-English tag, add a missing one."""
    reply = reply.strip().strip('"')
    m = re.match(r"\s*\[([^\]]*)\]\s*(.*)", reply, re.S)
    if not m:
        return f"{DEFAULT_TAG} {reply}"
    tag, line = m.group(1), m.group(2).strip()
    if not tag.isascii() or not tag.strip():
        print(f"[brain] non-English tag {tag!r} -> {DEFAULT_TAG}")
        return f"{DEFAULT_TAG} {line}"
    return f"[{tag}] {line}"

LANG_NAMES = {"en": "English", "ru": "Russian", "kk": "Kazakh", "zh": "Mandarin Chinese",
              "tr": "Turkish", "de": "German", "fr": "French", "es": "Spanish",
              "ja": "Japanese", "ko": "Korean", "uk": "Ukrainian", "it": "Italian"}

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
    print(f"[stt] loading whisper {WHISPER_MODEL!r} (first run downloads the model)...")
    model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
    print("[stt] ready")
    return model


def transcribe(model, audio: np.ndarray, language: str | None = None):
    """-> (text, lang). Language auto-detected unless pinned via WHISPER_LANG."""
    lang = language or os.environ.get("WHISPER_LANG") or None
    segments, info = model.transcribe(audio, language=lang, beam_size=1,
                                      vad_filter=False, condition_on_previous_text=False)
    text = " ".join(s.text.strip() for s in segments).strip()
    detected = info.language if lang is None else lang
    print(f"[stt] heard ({detected} {info.language_probability:.2f}): {text!r}")
    return text, detected


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


def think(brain, history: list, heard: str, lang: str | None = None) -> str:
    global canned_idx
    if brain is None:
        reply = CANNED[canned_idx % len(CANNED)]
        canned_idx += 1
        return reply
    if lang and lang != "en":
        name = LANG_NAMES.get(lang, lang)
        heard = f"{heard}\n(the human spoke {name} — sing your reply in {name})"
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
    reply = fix_tag(reply)
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
        heard, lang = transcribe(whisper, audio)
        if not heard:
            print("[stt] heard nothing, try again")
            continue
        try:
            reply = think(brain, history, heard, lang)
        except Exception as e:
            print(f"[err] think failed ({e}) — using canned line")
            global canned_idx
            reply = CANNED[canned_idx % len(CANNED)]
            canned_idx += 1
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
