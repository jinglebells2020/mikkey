# Mikkey — singing desk buddy

A M5StickS3 body + a laptop brain. Everything Mikkey says is sung (Fish Audio
S2.1). The stick records your question (hold the button), the laptop
transcribes it (faster-whisper), writes a reply in character (OpenRouter,
gpt-oss-120b), and streams the sung answer back with a mouth-sync envelope.

## Architecture

```
[M5StickS3]  <-- USB serial  OR  TCP over iPhone hotspot -->  [laptop]
  speaker/mic/LCD/button          mikkey_server.py :8090(http) :8091(tcp)
                                    whisper + LLM + Fish Audio APIs
```

- The **device is the authority over the link**: it prints `POLL` on whichever
  transport it is reading, and the server only pushes clips in reply to a POLL.
- Cached clips go as `MIKY` blobs (slurp to PSRAM, then play). Live answers
  stream as `MIKS` cells (thinking-hum filler -> answer, playback starts 0.5s in).
- USB CDC quirks (hard-won, do not regress): host writes must be paced
  ~250 kB/s or the CDC stalls permanently; the RX ring silently drops bytes if
  flooded while the firmware draws — streams are paced ~1.4x real-time.

## Filming day

```
./run.sh          # preflight + server with auto-restart + timestamps
```

1. Phone hotspot ON with **Maximize Compatibility** (ESP32 is 2.4GHz-only).
   Laptop on the hotspot. Keep the phone plugged in and near Mikkey.
2. Power the stick (battery lasts <1h — use a USB power bank for long takes;
   power-only is fine, data cable optional).
3. Blue dot top-left on the stick = TCP link up. Grey = USB fallback.
4. **Tap** the button = next script.yaml line. **Hold** = push-to-talk.
5. Everything Mikkey sang lives in `out/` (mp3 for scripted lines, wav for
   live answers) — those are the video-edit masters.

Failure behavior (all sung/visible, never silent): brain or TTS failure ->
sung apology; link loss -> clip requeued and replayed; errors show a face +
reason on the LCD; battery <20% shows a glyph, <10% blinks.

## Files

- `mikkey_server.py` — the whole host: HTTP (`/say`, `/trigger`), USB+TCP
  links, whisper+LLM, Fish streaming, quick replies, fillers, error clips.
- `firmware/mikkey/` — StickS3 firmware (face state machine, audio, mic, links).
  Flash: `arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB" firmware/mikkey && arduino-cli upload ... -p /dev/cu.usbmodem*`
  Config: copy `firmware/mikkey/wifi_config.h.example` -> `wifi_config.h`.
- `script.yaml` — the video's lines, in order (pre-generated at server start).
- `sing_test.py` / `audio_pipeline.py` — TTS test harness / offline PCM+envelope
  tools (`preview <id>` shows an ASCII mouth synced to laptop playback).
- `mikkey_talk.py` — laptop-mic push-to-talk (fallback if the stick mic fails).

Secrets: `.env` (FISH_API_KEY, OPENROUTER_API_KEY[, ANTHROPIC_API_KEY]) and
`firmware/mikkey/wifi_config.h` — both gitignored, never committed.
