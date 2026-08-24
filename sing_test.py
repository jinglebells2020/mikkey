#!/usr/bin/env python3
"""Mikkey Task 1: Fish Audio sung-delivery test harness.

Reads lines.yaml, generates each line via the Fish Audio API, saves MP3s to out/.

Usage:
  .venv/bin/python sing_test.py            # generate anything not already in out/
  .venv/bin/python sing_test.py --force    # regenerate everything
  .venv/bin/python sing_test.py --only sea-shanty
  .venv/bin/python sing_test.py --listen   # play results in order (macOS afplay)
"""

import argparse
import sys
import subprocess
import time
from pathlib import Path

import yaml
from dotenv import load_dotenv
from fishaudio import FishAudio

ROOT = Path(__file__).parent
OUT = ROOT / "out"


def load_lines():
    data = yaml.safe_load((ROOT / "lines.yaml").read_text())
    return data


def generate(args):
    load_dotenv(ROOT / ".env")
    data = load_lines()
    model = data.get("model", "s2.1-pro-free")
    reference_id = data.get("reference_id") or None
    lines = data["lines"]
    if args.only:
        lines = [l for l in lines if l["id"] == args.only]
        if not lines:
            sys.exit(f"no line with id {args.only!r} in lines.yaml")

    OUT.mkdir(exist_ok=True)
    client = FishAudio()  # reads FISH_API_KEY
    print(f"model: {model}   voice: {reference_id or '(default)'}   lines: {len(lines)}\n")

    results = []
    for line in lines:
        lid, text = line["id"], line["text"]
        path = OUT / f"{lid}.mp3"
        if path.exists() and not args.force and not args.only:
            results.append((lid, "cached", path.stat().st_size, 0.0))
            print(f"  {lid:20s} cached ({path.stat().st_size} bytes)")
            continue
        t0 = time.time()
        try:
            audio = client.tts.convert(text=text, model=model, format="mp3",
                                       reference_id=reference_id)
            path.write_bytes(audio)
            dt = time.time() - t0
            results.append((lid, "ok", len(audio), dt))
            print(f"  {lid:20s} ok     {len(audio):7d} bytes  {dt:5.1f}s")
        except Exception as e:
            dt = time.time() - t0
            results.append((lid, f"FAIL: {e}", 0, dt))
            print(f"  {lid:20s} FAIL   {e}")

    print("\n--- summary ---")
    ok = [r for r in results if r[1] in ("ok", "cached")]
    bad = [r for r in results if r not in ok]
    print(f"{len(ok)} ok, {len(bad)} failed. Files in {OUT}/")
    print("listen in order:  .venv/bin/python sing_test.py --listen")
    return 0 if not bad else 1


def listen(args):
    data = load_lines()
    for line in data["lines"]:
        lid = line["id"]
        path = OUT / f"{lid}.mp3"
        if not path.exists():
            print(f"  {lid:20s} (missing, skipped)")
            continue
        print(f"\n▶ {lid}\n  {line['text']}")
        subprocess.run(["afplay", str(path)])
    return 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--force", action="store_true", help="regenerate even if cached")
    p.add_argument("--only", help="generate a single line id")
    p.add_argument("--listen", action="store_true", help="play generated files in order")
    args = p.parse_args()
    sys.exit(listen(args) if args.listen else generate(args))


if __name__ == "__main__":
    main()
