# Mikkey — singing desk buddy

A M5StickS3 body on a keychain + a laptop brain. Everything Mikkey says is sung (Fish Audio
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

## Filming day (Fish Audio reel — script v5)

```
./run.sh                       # preflight + server with auto-restart + timestamps
open http://localhost:8090/studio   # the on-screen tool for the emotion-tag beat
```

1. Network: the stick joins the SSID in `firmware/mikkey/wifi_config.h`
   (`./set_wifi.sh "<ssid>" "<pass>"` rewrites it and reflashes). Home routers
   with band steering / weak 2.4 GHz can refuse the stick (see below); the
   phone hotspot with **Maximize Compatibility** is the known-good fallback.
2. Power the stick (battery lasts <1h — USB power bank for long takes).
3. Blue dot top-left = TCP link up. Grey = USB fallback.
4. **Tap** = next `script.yaml` line, in shoot order. **Hold** = push-to-talk.
   `/studio` can sing any line directly or move the cursor (shoot out of order).
5. Masters: every line Mikkey sang is in `out/` (mp3 for scripted, wav for live
   and canned) and `out/takes.log` says what played when, with the file id.

Beat by beat:

- **Hook (0:00)** — hold the button and ask "Mikkey, say something normal":
  any sentence with *normal / talk / speak* fires the cached hook line
  instantly (also line 1 on tap). Picking him up while he naps plays the
  wake-up ritual (hat flips on) instead of a startle — that is the
  "screen lights up" shot; let him nap ~60 s first.
- **Emotion tags (0:23)** — record `/studio`: two big fields, *direction* and
  *line*. Sing, change only the direction, Sing again. Masters land in
  `out/say-<time>.mp3`. Lines `tag-broadway` / `tag-lullaby` are pre-generated
  as tap fallbacks.
- **He hears you (0:36)** — hold, friend speaks Russian, release. Whisper
  auto-detects Russian and Mandarin reliably; **Kazakh is heard as Turkish**,
  so pin `kk` on `/studio` before that cut (then back to `auto`). The reply is
  sung in the speaker's language; the bracket tag is forced to English.
  Scripted `ru-hello` / `kk-hello` / `zh-hello` exist as fallbacks.
- **BIG_FACE** (firmware, default): portrait screen, bare head flush with the
  hat end so the 3D-printed hat sits on his head in every state; body below.
  `FACE_ROTATION` 0/2 picks which short end is "up". Set `BIG_FACE 0` for the
  old landscape pop-star scenes.
- **SHOOT_MODE** (firmware): handling never makes him grumpy; naps still happen.

Failure behavior (all sung/visible, never silent): brain or TTS failure ->
sung apology; link loss -> clip requeued and replayed; errors show a face +
reason on the LCD; battery <20% shows a glyph, <10% blinks.

WiFi diagnostics: the stick prints `dbg wifi ...` on USB serial — a scan of
what it sees, the disconnect reason (2/4 = AP not answering, 15 = WPA2
handshake timeout) and its IP once joined.

## Files

- `mikkey_server.py` — the whole host: HTTP (`/say`, `/trigger`), USB+TCP
  links, whisper+LLM, Fish streaming, quick replies, fillers, error clips.
- `firmware/mikkey/` — StickS3 firmware (bigface.h portrait face for the hat,
  dragon.h legacy landscape scenes, audio, mic, links).
  Flash: `arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB" firmware/mikkey && arduino-cli upload ... -p /dev/cu.usbmodem*`
  Config: copy `firmware/mikkey/wifi_config.h.example` -> `wifi_config.h`.
- `script.yaml` — the reel's lines, in shoot order (pre-generated at server start;
  changing a line's text regenerates it).
- `set_wifi.sh` — rewrite wifi_config.h for a new SSID and reflash.
- `sing_test.py` / `audio_pipeline.py` — TTS test harness / offline PCM+envelope
  tools (`preview <id>` shows an ASCII mouth synced to laptop playback).
- `mikkey_talk.py` — laptop-mic push-to-talk (fallback if the stick mic fails).

Secrets: `.env` (FISH_API_KEY, OPENROUTER_API_KEY[, ANTHROPIC_API_KEY]) and
`firmware/mikkey/wifi_config.h` — both gitignored, never committed.
