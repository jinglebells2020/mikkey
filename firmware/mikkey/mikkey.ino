// Mikkey firmware for M5StickS3 — dragon edition.
//
// Link policy (single source of truth = this device):
//   - If WiFi joins, TCP is THE protocol link for the session; USB serial is
//     debug-out + drain-discarded. Server pushes only in response to POLL.
// Protocol: "MIKY" fixed clips (slurp to PSRAM then play), "MIKS" streamed
//   cells ('D' + env + 882B PCM, 'E' ends). Upstream: READY/POLL/TRIG/REC/
//   "MIC n"+PCM/PLAY/DONE/UNDERRUN/dbg.
// Presentation: a tamagotchi pixel dragon (dragon.h) that obeys real gravity
//   from the BMI270 — stands on the true "down" edge, tumbles on tilt,
//   startles on shake, flies against gravity, naps in a cave, and lip-syncs
//   the envelope with a big close-up head while singing.

// SHOOT_MODE: filming build. Handling him between takes must never leave him
// grumpy/fainted on camera, and picking him up while he naps plays the hat
// wake-up ritual (the hook shot) instead of a startle.
#define SHOOT_MODE 1
// BIG_FACE: portrait big-head presentation for the 3D-printed hat (bigface.h).
// FACE_ROTATION 0 puts the head at one short end of the stick, 2 at the other —
// pick the end the hat sits on. 0 = classic landscape pop-star scenes (dragon.h).
#define BIG_FACE 1
#define FACE_ROTATION 0

#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "wifi_config.h"
#include "face_types.h"
#if BIG_FACE
#include "bigface.h"
#else
#include "dragon.h"
#endif

// ---------------------------------------------------------------- link ----
static WiFiClient tcp;
static Stream *link_ = &Serial;
static bool tcpUp = false;
static bool tcpEverUp = false;     // sticky: once TCP wins, serial is debug-only
static uint32_t lastNetTry = 0;
static IPAddress serverIp;
static bool serverIpKnown = false;
static uint32_t lastPoll = 0;
static bool playing = false;

static void chirp(int f1, int f2) {
  M5.Speaker.tone(f1, 40); delay(45);
  if (f2) { M5.Speaker.tone(f2, 60); delay(65); }
}

static void drainStream(Stream *s) {
  uint8_t junk[64];
  while (s->available() > 0) s->readBytes((char *)junk, min(s->available(), 64));
}

static void linkWriteAll(const uint8_t *p, size_t n) {
  int stalls = 0;
  while (n > 0 && stalls < 2000) {
    size_t w = link_->write(p, n);
    if (w == 0) { stalls++; delay(1); continue; }
    stalls = 0;
    p += w;
    n -= w;
  }
  if (n > 0) Serial.printf("dbg linkWriteAll gave up with %u left\n", (unsigned)n);
}

// -------------------------------------------------------------- protocol ----
static const uint32_t CHUNK = 441;
static const size_t ENV_MAX = 16384;
static const size_t PCM_MAX = 4 * 1024 * 1024;
static uint8_t envBuf[ENV_MAX];
static uint32_t lastDrawUs = 0;
static int16_t *clipBuf = nullptr;

static void pollButton() {
  M5.update();
  if (M5.BtnA.wasClicked()) {
    M5.Speaker.tone(1200, 30);
    link_->println("TRIG");
    if (faceState == FACE_IDLE || faceState == FACE_ERROR) faceSetState(FACE_THINK);
  }
}

static bool readExact(uint8_t *dst, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < n) {
    int avail = link_->available();
    if (avail > 0) {
      size_t take = ((size_t)avail < n - got) ? (size_t)avail : n - got;
      got += link_->readBytes((char *)dst + got, take);
      last = millis();
    } else {
      if (millis() - last > timeout_ms) return false;
      pollButton();
      delay(1);
    }
  }
  return true;
}

static void playClip(uint32_t rate, uint32_t nSamples, uint32_t nEnv) {
  Serial.printf("dbg clip: %lu samples @ %lu Hz, %lu env frames\n",
                (unsigned long)nSamples, (unsigned long)rate, (unsigned long)nEnv);
  if (nEnv > ENV_MAX || nSamples * 2 > PCM_MAX || !clipBuf) {
    faceError("clip too big");
    link_->println("DONE 0");
    return;
  }
  if (!readExact(envBuf, nEnv, 3000)) {
    faceError("env timeout");
    link_->println("DONE 0");
    return;
  }
  uint32_t t0 = millis();
  if (!readExact((uint8_t *)clipBuf, nSamples * 2, 4000)) {
    faceError("pcm timeout");
    link_->println("DONE 0");
    return;
  }
  Serial.printf("dbg slurped %lu bytes in %lu ms\n",
                (unsigned long)(nSamples * 2), (unsigned long)(millis() - t0));
  link_->println("PLAY");

  playing = true;
  uint32_t framesDone = 0, underruns = 0, remaining = nSamples;
  bool spkStarted = false;   // the speaker task takes a few frames to spin up
  while (remaining > 0) {
    uint32_t take = remaining < CHUNK ? remaining : CHUNK;
    while (M5.Speaker.isPlaying(0) >= 2) { pollButton(); delay(1); }
    if (M5.Speaker.isPlaying(0) > 0) spkStarted = true;
    else if (spkStarted) {
      underruns++;   // real starvation mid-clip: audible gap
      if (underruns <= 3) Serial.printf("dbg underrun at frame %lu (draw %lu us)\n",
                                        (unsigned long)framesDone, (unsigned long)lastDrawUs);
    }
    M5.Speaker.playRaw(clipBuf + framesDone * CHUNK, take, rate, false, 1, 0);
    if (framesDone < nEnv) {
      uint32_t d0 = micros();
      singMouthFrame(envBuf[framesDone]);
      lastDrawUs = micros() - d0;
    }
    framesDone++;
    remaining -= take;
  }
  while (M5.Speaker.isPlaying(0)) { pollButton(); delay(5); }
  playing = false;
  if (underruns) link_->printf("UNDERRUN %lu\n", (unsigned long)underruns);
  link_->printf("DONE %lu\n", (unsigned long)framesDone);
  faceSetState(FACE_IDLE);   // dragon plays the fire-breath flourish
}

static const uint32_t PREBUFFER_CELLS = 25;

static void playClipStream(uint32_t rate) {
  Serial.printf("dbg stream start @ %lu Hz\n", (unsigned long)rate);
  if (!clipBuf) { faceError("no buffer"); link_->println("DONE 0"); return; }
  playing = true;
  uint32_t cells = 0, played = 0, underruns = 0, badTags = 0;
  bool done = false, started = false;
  uint32_t lastData = millis();

  bool spkStarted = false;
  while (true) {
    while (!done && link_->available()) {
      int tc = link_->read();
      if (tc < 0) break;
      uint8_t tag = (uint8_t)tc;
      if (tag == 'D') {
        if (cells >= ENV_MAX || (size_t)(cells + 1) * 441 * 2 > PCM_MAX) {
          Serial.println("dbg stream overflow, ending early");
          done = true;
        } else if (!readExact(&envBuf[cells], 1, 3000) ||
                   !readExact((uint8_t *)(clipBuf + (size_t)cells * 441), 882, 3000)) {
          Serial.println("dbg stream cell timeout");
          done = true;
        } else {
          cells++;
          lastData = millis();
        }
      } else if (tag == 'E') {
        Serial.printf("dbg E at cell %lu\n", (unsigned long)cells);
        done = true;
      } else if (tag == 'M') {
        // in-stream mood tag: one byte, colors the jaw tint + the flourish
        int mv = -1;
        uint32_t tw = millis();
        while (mv < 0 && millis() - tw < 200) { mv = link_->read(); if (mv < 0) delay(1); }
        if (mv >= 0 && mv <= 3) dragonSetMood((uint8_t)mv);
      } else {
        badTags++;
      }
    }
    if (!done && millis() - lastData > 6000) {
      Serial.println("dbg stream stalled, ending");
      done = true;
    }
    if (!started && (cells >= PREBUFFER_CELLS || (done && cells > 0))) {
      started = true;
      link_->println("PLAY");
    }
    if (started && played < cells && M5.Speaker.isPlaying(0) < 2) {
      if (M5.Speaker.isPlaying(0) > 0) spkStarted = true;
      else if (spkStarted) underruns++;
      M5.Speaker.playRaw(clipBuf + (size_t)played * 441, 441, rate, false, 1, 0);
      if ((played & 1) == 0) singMouthFrame(envBuf[played]);
      played++;
    }
    if (done && played >= cells && !M5.Speaker.isPlaying(0)) break;
    if (done && cells == 0) break;
    pollButton();
    delay(1);
  }
  playing = false;
  if (badTags > 8) faceError("bad tags");
  if (underruns) link_->printf("UNDERRUN %lu\n", (unsigned long)underruns);
  link_->printf("DONE %lu\n", (unsigned long)played);
  if (badTags <= 8) faceSetState(FACE_IDLE);
}

// hold BtnA = push-to-talk
static const uint32_t MIC_RATE = 16000;
static const size_t MIC_CHUNK = 512;

static void recordAndSend() {
  if (!clipBuf) { faceError("no buffer"); return; }
  faceSetState(FACE_LISTEN);
  link_->println("REC");
  M5.Speaker.setVolume(0); delay(15);
  M5.Speaker.end();
  M5.Mic.begin();

  int16_t *mbuf = clipBuf;
  size_t total = 0, maxSamples = PCM_MAX / 2;
  while (total + MIC_CHUNK <= maxSamples) {
    M5.update();
    if (!M5.BtnA.isPressed()) break;
    M5.Mic.record(mbuf + total, MIC_CHUNK, MIC_RATE);
    total += MIC_CHUNK;
    if (total >= MIC_CHUNK) faceListenTick(mbuf + total - MIC_CHUNK, MIC_CHUNK);
  }
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
  M5.Speaker.begin();
  delay(15); M5.Speaker.setVolume(255);

  link_->printf("MIC %u\n", (unsigned)total);
  const uint8_t *p = (const uint8_t *)mbuf;
  for (size_t off = 0; off < total * 2; off += 4096) {
    size_t n = total * 2 - off;
    linkWriteAll(p + off, n < 4096 ? n : 4096);
  }
  Serial.printf("dbg mic sent %u samples (%.1fs)\n", (unsigned)total,
                total / (float)MIC_RATE);
  delay(30);
  drainStream(link_);
  faceSetState(FACE_THINK);
}

// ------------------------------------------------------------------ net ----
static void tryNet() {
  if (playing || faceState == FACE_LISTEN) return;
  if (link_->available() > 0) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (tcpUp) { tcpUp = false; dragonNetUp = false; if (!tcpEverUp) link_ = &Serial; }
    // A home router (scan + WPA2 + DHCP) can take >10s to join; restarting
    // the attempt every 8s ("sta is connecting, cannot set config") never
    // let it finish. Only kick a definitively failed/stalled attempt.
    static uint32_t lastStatusLog = 0;
    wl_status_t st = WiFi.status();
    if (millis() - lastStatusLog > 5000) {
      lastStatusLog = millis();
      Serial.printf("dbg wifi status=%d (waiting for %s)\n", (int)st, WIFI_SSID);
    }
    // one-time async scan on the first failure: what does the stick see?
    static int8_t scanState = 0;             // 0 idle, 1 running, 2 done
    if (scanState == 0 && millis() - lastNetTry > 12000) {
      scanState = 1;
      WiFi.scanNetworks(true, false, false, 300);
    } else if (scanState == 1) {
      int n = WiFi.scanComplete();
      if (n >= 0) {
        scanState = 2;
        Serial.printf("dbg wifi scan: %d networks\n", n);
        for (int i = 0; i < n && i < 12; i++)
          Serial.printf("dbg   %-28s ch=%2d rssi=%4d auth=%d%s\n", WiFi.SSID(i).c_str(),
                        WiFi.channel(i), WiFi.RSSI(i), (int)WiFi.encryptionType(i),
                        WiFi.SSID(i) == WIFI_SSID ? "  <-- target" : "");
        WiFi.scanDelete();
        WiFi.begin(WIFI_SSID, WIFI_PASS);    // scan aborted the attempt; restart it
        lastNetTry = millis();
      }
    }
    bool failed = (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
                   st == WL_CONNECTION_LOST || st == WL_DISCONNECTED);
    if ((failed && millis() - lastNetTry > 15000) || millis() - lastNetTry > 30000) {
      lastNetTry = millis();
      Serial.println("dbg wifi (re)connecting...");
      WiFi.disconnect();
      delay(50);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (tcp.connected()) {
    if (!tcpUp) {
      tcpUp = true;
      dragonNetUp = true;
      if (!tcpEverUp) chirp(900, 1400);
      tcpEverUp = true;
      link_ = &tcp;
    }
    return;
  }
  if (tcpUp) {
    tcpUp = false;
    dragonNetUp = false;
    chirp(1400, 900);
    Serial.println("dbg tcp lost");
    // sticky: never fall back to serial protocol once TCP owned the link
  }
  if (millis() - lastNetTry > 4000) {
    lastNetTry = millis();
    WiFi.setSleep(false);
    if (!serverIpKnown) {
      IPAddress ip;
      ip.fromString(SERVER_IP_FALLBACK);
      serverIp = ip;
      IPAddress m = MDNS.queryHost(SERVER_HOST);
      if (m != IPAddress()) serverIp = m;
      serverIpKnown = true;
    }
    Serial.printf("dbg tcp connecting to %s:%d\n", serverIp.toString().c_str(), SERVER_PORT);
    if (tcp.connect(serverIp, SERVER_PORT, 1500)) {
      tcp.setNoDelay(true);
      tcpUp = true;
      tcpEverUp = true;
      dragonNetUp = true;
      link_ = &tcp;
      drainStream(&Serial);
      link_->printf("READY tcp rssi=%d bat=%d\n", WiFi.RSSI(), M5.Power.getBatteryLevel());
      Serial.println("dbg tcp connected");
    } else {
      serverIpKnown = false;
    }
  }
}

// ---------------------------------------------------------------- setup ----
static const uint8_t MAGIC[4] = {'M', 'I', 'K', 'Y'};

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(255);
#if BIG_FACE
  M5.Display.setRotation(FACE_ROTATION);
#else
  M5.Display.setRotation(1);
#endif
  M5.Display.setBrightness(200);

  clipBuf = (int16_t *)ps_malloc(PCM_MAX);

  Serial.setRxBufferSize(32768);
  Serial.begin(115200);
  delay(300);
  if (!clipBuf) Serial.println("dbg FATAL: ps_malloc failed");
  Serial.printf("READY board=%d psram=%u\n", (int)M5.getBoard(), ESP.getPsramSize());

  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      // rate-limited: on a refusing AP this fires every second, and serial
      // spam from the WiFi task steals time from playback (underruns)
      static uint32_t lastLog = 0;
      static int lastReason = -1;
      int r = (int)info.wifi_sta_disconnected.reason;
      if (r != lastReason || millis() - lastLog > 10000) {
        lastLog = millis(); lastReason = r;
        Serial.printf("dbg wifi disconnected reason=%d\n", r);
      }
    }
    else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.printf("dbg wifi ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED)
      Serial.printf("dbg wifi associated ch=%d\n", (int)info.wifi_sta_connected.channel);
  });
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);   // tryNet() retries, never during playback/listen
  WiFi.setTxPower(WIFI_POWER_19_5dBm);   // tiny PCB antenna: every dB counts
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastNetTry = millis();
  MDNS.begin("mikkey");

  dragonInit();          // IMU calibration (~320ms) + hatch animation
  faceSetState(FACE_BOOT);
  M5.Speaker.tone(660, 70); delay(80);
  M5.Speaker.tone(880, 70); delay(80);
  M5.Speaker.tone(1320, 110);
}

void loop() {
  pollButton();
  faceTick();
  if (M5.BtnA.wasHold()) { recordAndSend(); return; }
  tryNet();

  if (!playing && faceState != FACE_LISTEN && !M5.BtnA.isPressed() &&
      millis() - lastPoll > 500) {
    lastPoll = millis();
    link_->printf("POLL bat=%d rssi=%d\n", M5.Power.getBatteryLevel(),
                  tcpUp ? WiFi.RSSI() : 0);
  }

  if (tcpEverUp && link_ == &tcp) drainStream(&Serial);

  static int magicPos = 0;
  while (link_->available()) {
    int c = link_->read();
    if (c < 0) break;
    uint8_t b = (uint8_t)c;
    if (magicPos < 3) {
      magicPos = (b == MAGIC[magicPos]) ? magicPos + 1 : (b == MAGIC[0] ? 1 : 0);
      continue;
    }
    magicPos = 0;
    if (b == 'Y') {
      uint8_t hdr[12];
      if (!readExact(hdr, 12, 2000)) { faceError("hdr timeout"); return; }
      uint32_t rate, nSamples, nEnv;
      memcpy(&rate, hdr, 4); memcpy(&nSamples, hdr + 4, 4); memcpy(&nEnv, hdr + 8, 4);
      if (rate < 8000 || rate > 48000 || nSamples > 60UL * 48000) {
        faceError("bad header");
        return;
      }
      faceSetState(FACE_SING);
      playClip(rate, nSamples, nEnv);
    } else if (b == 'S') {
      uint8_t hdr[4];
      if (!readExact(hdr, 4, 2000)) { faceError("hdr timeout"); return; }
      uint32_t rate;
      memcpy(&rate, hdr, 4);
      if (rate < 8000 || rate > 48000) { faceError("bad rate"); return; }
      faceSetState(FACE_SING);
      playClipStream(rate);
    }
  }
  delay(2);
}
