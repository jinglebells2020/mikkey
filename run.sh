#!/bin/bash
# Mikkey launcher: preflight checks, then run the server forever with
# auto-restart and timestamped logs. This is the only command filming needs.
cd "$(dirname "$0")"

if [ ! -f .env ]; then
  echo "FATAL: no .env — copy .env.example and fill in FISH_API_KEY (+ OPENROUTER_API_KEY)"
  exit 1
fi
grep -q FISH_API_KEY .env || { echo "FATAL: FISH_API_KEY missing from .env"; exit 1; }
[ -d .venv ] || { echo "FATAL: no .venv — python3 -m venv .venv && .venv/bin/pip install -r requirements.txt"; exit 1; }

echo "== mikkey preflight =="
grep -q OPENROUTER_API_KEY .env && echo "brain: openrouter" || echo "brain: CANNED ONLY (no OPENROUTER_API_KEY)"
ls /dev/cu.usbmodem* 2>/dev/null && echo "usb: stick cable present" || echo "usb: no cable (WiFi/TCP mode)"
echo "laptop ip: $(ipconfig getifaddr en0 2>/dev/null || echo '?') (stick's wifi_config.h must point here or resolve mDNS '$(scutil --get LocalHostName 2>/dev/null)')"
echo "======================"

while true; do
  .venv/bin/python -u mikkey_server.py 2>&1 | while IFS= read -r line; do
    printf '%s %s\n' "$(date +%H:%M:%S)" "$line"
  done
  echo "$(date +%H:%M:%S) [run.sh] SERVER EXITED — restarting in 2s (Ctrl+C twice to stop)"
  sleep 2
done
