#!/bin/bash
# Point Mikkey at a WiFi network and reflash. Usage:
#   ./set_wifi.sh "<ssid>" "<password>" [server-ip]
# server-ip defaults to this laptop's current IP (the stick tries mDNS first).
set -e
cd "$(dirname "$0")"
[ $# -ge 2 ] || { echo "usage: $0 <ssid> <password> [server-ip]"; exit 1; }
IP="${3:-$(ipconfig getifaddr en0 2>/dev/null || echo 172.20.10.7)}"
HOST="$(scutil --get LocalHostName 2>/dev/null || echo enes)"
cat > firmware/mikkey/wifi_config.h <<CFG
// WiFi + server config. This file is gitignored — never commit credentials.
#pragma once
#define WIFI_SSID "$1"
#define WIFI_PASS "$2"
#define SERVER_HOST "$HOST"              // laptop's mDNS name (scutil --get LocalHostName)
#define SERVER_IP_FALLBACK "$IP"  // laptop's IP if mDNS fails
#define SERVER_PORT 8091
CFG
echo "wifi_config.h: ssid=$1 server=$HOST / $IP"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB"
PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
[ -n "$PORT" ] || { echo "no stick on USB (/dev/cu.usbmodem*) — plug it in"; exit 1; }
arduino-cli compile --fqbn "$FQBN" firmware/mikkey | grep -E "error|Sketch uses"
arduino-cli upload --fqbn "$FQBN" -p "$PORT" firmware/mikkey | grep -E -i "error|Hard resetting"
echo "flashed. Blue dot top-left = TCP link up."
