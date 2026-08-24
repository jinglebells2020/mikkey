#!/usr/bin/env python3
"""Mikkey Task 2: PCM conversion + mouth envelope + ASCII sync preview.

For each MP3 in out/, produces in cache/:
  {id}.pcm  - 16-bit signed LE mono 22050 Hz raw PCM
  {id}.env  - amplitude envelope, one unsigned byte per 441-sample window (50/s)

The original out/*.mp3 files are never touched — they go into the video edit.

Usage:
  .venv/bin/python audio_pipeline.py build            # process all out/*.mp3
  .venv/bin/python audio_pipeline.py build --force
  .venv/bin/python audio_pipeline.py preview <id>     # play + ASCII mouth
  .venv/bin/python audio_pipeline.py preview <id> --nosound
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).parent
OUT = ROOT / "out"
CACHE = ROOT / "cache"

SAMPLE_RATE = 22050
WINDOW = 441          # samples per envelope frame -> exactly 50 frames/sec
FPS = SAMPLE_RATE / WINDOW

ATTACK = 0.7          # how fast the mouth opens  (fraction of gap closed per frame)
DECAY = 0.25          # how fast it closes
FLOOR = 0.15          # min openness while sound is present
SILENCE_RMS = 0.02    # below this (normalised) counts as real silence -> mouth may close


def mp3_to_pcm(mp3: Path, pcm: Path):
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(mp3),
         "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "s16le", str(pcm)],
        check=True)


def compute_envelope(pcm_path: Path) -> np.ndarray:
    samples = np.frombuffer(pcm_path.read_bytes(), dtype=np.int16).astype(np.float64)
    n_win = len(samples) // WINDOW
    if n_win == 0:
        return np.zeros(0, dtype=np.uint8)
    windows = samples[: n_win * WINDOW].reshape(n_win, WINDOW)
    rms = np.sqrt((windows ** 2).mean(axis=1))
    peak = rms.max()
    raw = rms / peak if peak > 0 else rms

    # fast attack / slow decay, with a floor while sound is present
    env = np.zeros(n_win)
    cur = 0.0
    for i, target in enumerate(raw):
        coef = ATTACK if target > cur else DECAY
        cur += coef * (target - cur)
        val = cur
        if raw[i] > SILENCE_RMS:
            val = max(val, FLOOR)
        env[i] = val

    return np.clip(env * 255, 0, 255).astype(np.uint8)


def build(args):
    CACHE.mkdir(exist_ok=True)
    mp3s = sorted(OUT.glob("*.mp3"))
    if not mp3s:
        sys.exit("no mp3s in out/ — run sing_test.py first")
    for mp3 in mp3s:
        lid = mp3.stem
        pcm, env = CACHE / f"{lid}.pcm", CACHE / f"{lid}.env"
        if pcm.exists() and env.exists() and not args.force:
            print(f"  {lid:20s} cached")
            continue
        mp3_to_pcm(mp3, pcm)
        e = compute_envelope(pcm)
        env.write_bytes(e.tobytes())
        secs = (pcm.stat().st_size // 2) / SAMPLE_RATE
        print(f"  {lid:20s} {secs:5.1f}s  {len(e)} env frames  "
              f"(mean {e.mean()/255:.2f}, peak {e.max()/255:.2f})")
    print(f"\ndone. preview:  .venv/bin/python audio_pipeline.py preview <id>")


MOUTH_W = 40

def draw_mouth(openness: float):
    lit = int(round(openness * MOUTH_W))
    pad = (MOUTH_W - lit) // 2
    bar = " " * pad + "#" * lit + " " * (MOUTH_W - pad - lit)
    sys.stdout.write(f"\r  [{bar}]  {openness:4.2f} ")
    sys.stdout.flush()


def preview(args):
    pcm, envf = CACHE / f"{args.id}.pcm", CACHE / f"{args.id}.env"
    mp3 = OUT / f"{args.id}.mp3"
    if not envf.exists():
        sys.exit(f"no cache for {args.id!r} — run: audio_pipeline.py build")
    env = np.frombuffer(envf.read_bytes(), dtype=np.uint8) / 255.0
    print(f"previewing {args.id}: {len(env)} frames, {len(env)/FPS:.1f}s")

    player = None
    if not args.nosound:
        player = subprocess.Popen(["afplay", str(mp3)])
    t0 = time.time()
    for i in range(len(env)):
        target = t0 + i / FPS
        dt = target - time.time()
        if dt > 0:
            time.sleep(dt)
        draw_mouth(float(env[i]))
    draw_mouth(0.0)
    print()
    if player:
        player.wait()


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--force", action="store_true")
    b.set_defaults(func=build)
    v = sub.add_parser("preview")
    v.add_argument("id")
    v.add_argument("--nosound", action="store_true")
    v.set_defaults(func=preview)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
