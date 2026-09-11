#!/usr/bin/env python3
"""Mikkey: type a line, he sings it.

The terminal twin of /studio — whatever you type goes to POST /say and comes
out of the stick. Plain text gets the standing direction prepended; your own
[tag] wins for that line only. The script.yaml lines are one keystroke away,
so the reel can be shot from the keyboard instead of the button.

Needs mikkey_server.py running (./run.sh) and the stick on USB or TCP.

Run:  .venv/bin/python mikkey_type.py
"""

import json
import readline  # noqa: F401 — up-arrow recalls a line for the next take
import sys
import textwrap
import urllib.error
import urllib.request

SERVER = "http://localhost:8090"
DEFAULT_DIR = "sung, cheerful, playful"

HELP = """\
lines
  <text>                sing it with the standing direction
  [breathy] <text>      sing it with this direction, this line only
commands
  /dir <words>          set the standing direction (/dir alone prints it)
  /ls                   the script, numbered, cursor marked
  /l <n|id>             sing a script line exactly as written
  /next                 sing the line at the cursor, then advance (= a tap)
  /goto <n>             move the cursor
  /help   /q            this, and quit (ctrl-C clears the line)"""


def api(path: str, payload: dict | None = None, timeout: int = 120):
    """GET if payload is None, else POST. Raises RuntimeError with the
    server's own reason — generation failures come back as JSON on a 500."""
    req = urllib.request.Request(
        f"{SERVER}{path}",
        method="POST" if payload is not None else "GET",
        data=json.dumps(payload).encode() if payload is not None else None,
        headers={"Content-Type": "application/json"} if payload is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        try:
            msg = json.loads(raw).get("error", raw)
        except ValueError:
            msg = raw.strip() or f"HTTP {e.code}"
        raise RuntimeError(msg) from None


def compose(text: str, direction: str) -> str:
    """Fish reads the direction in [brackets]; a typed one wins for this line."""
    text = text.strip()
    return text if text.startswith("[") else f"[{direction}] {text}"


def say(text: str):
    res = api("/say", {"text": text})
    print(f"  -> {res['id']}")


def sing_script_line(arg: str):
    """Sing a script line by number or id. The id goes with it so the server
    uses script.yaml's own text and keeps the master's file name."""
    lines = api("/script")["lines"]
    if arg.isdigit():
        n = int(arg)
        if not 1 <= n <= len(lines):
            print(f"  no line {n} (1..{len(lines)}) — /ls")
            return
        line = lines[n - 1]
    else:
        line = next((l for l in lines if l["id"] == arg), None) \
            or next((l for l in lines if l["id"].startswith(arg)), None)
        if line is None:
            print(f"  no line {arg!r} — /ls")
            return
    print(f"  {line['id']}: {textwrap.shorten(line['text'], 70, placeholder=' ...')}")
    say_res = api("/say", {"id": line["id"], "text": line["text"]})
    print(f"  -> {say_res['id']}")


def show_script():
    state = api("/script")
    lines = state["lines"]
    pos = state["pos"] % len(lines)
    for i, line in enumerate(lines):
        mark = "->" if i == pos else "  "
        print(f" {mark} {i + 1:2}. {line['id']:<14} "
              f"{textwrap.shorten(line['text'], 62, placeholder=' ...')}")
    print(f"    cursor {pos + 1}/{len(lines)}, mic language {state['lang']}")


def main():
    try:
        with urllib.request.urlopen(f"{SERVER}/", timeout=3) as r:
            print(f"[server] {r.read().decode().strip()}")
    except Exception:
        sys.exit("mikkey_server.py is not running — start it first (./run.sh)")

    direction = DEFAULT_DIR
    print(f"\n=== type a line, Mikkey sings it. /help for commands. ===")
    print(f"direction: [{direction}]")
    while True:
        try:
            raw = input("\nmikkey> ").strip()
        except KeyboardInterrupt:
            print("  (/q to quit)")
            continue
        except EOFError:
            break
        if not raw:
            continue

        if raw.startswith("/"):
            cmd, _, arg = raw[1:].partition(" ")
            cmd, arg = cmd.lower(), arg.strip()
            try:
                if cmd in ("q", "quit", "exit"):
                    break
                elif cmd in ("help", "h", "?"):
                    print(HELP)
                elif cmd == "dir":
                    if arg:
                        direction = arg.strip("[]").strip()
                    print(f"  direction: [{direction}]")
                elif cmd == "ls":
                    show_script()
                elif cmd == "l":
                    if arg:
                        sing_script_line(arg)
                    else:
                        show_script()
                elif cmd == "next":
                    res = api("/trigger", {})
                    state = api("/script")
                    nxt = state["lines"][state["pos"] % len(state["lines"])]["id"]
                    print(f"  -> {res['id']}  (next {nxt})")
                elif cmd == "goto":
                    res = api("/goto", {"pos": int(arg) - 1})
                    print(f"  cursor {res['pos'] + 1}, next {res['next']}")
                else:
                    print(f"  no command /{cmd} — /help")
            except Exception as e:
                print(f"  [err] {e}")
            continue

        line = compose(raw, direction)
        print(f"  {line}")
        try:
            say(line)
        except Exception as e:
            print(f"  [err] {e}")


if __name__ == "__main__":
    main()
    print("\nbye")
