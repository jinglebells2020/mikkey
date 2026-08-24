// Mikkey firmware for M5StickS3 — production edition.
//
// Link policy (single source of truth = this device):
//   - If WiFi joins within 20s of boot, TCP is THE protocol link for the whole
//     session; USB serial is debug-out + drain-discarded. Otherwise serial is
//     the link (TCP may still claim it once, later, with a loud announce).
//   - When idle (not recording, not playing, button up) the device prints
//     "POLL bat=<pct> rssi=<dbm>" every 500ms on the link. The server only
//     pushes clips in response to a POLL — a push is therefore always bound
//     to the link the device is actually reading. POLL doubles as heartbeat.
//
// Protocol (host->device): "MIKY" + u32 rate + u32 samples + u32 envlen,
//   then env bytes then s16le PCM (slurped whole into PSRAM, then played).
//   "MIKS" + u32 rate, then cells of 'D' + env byte + 882B PCM, ended by 'E'
//   (played while receiving, 0.5s prebuffer).
// Device->host lines: READY / POLL / TRIG / REC / "MIC n"+raw PCM / PLAY /
//   DONE n / UNDERRUN n / dbg ...
//
// Face: sprite-based state machine — BOOT / IDLE (blinks, pupil wander,
//   breathing, sleep after 45s, IMU pickup surprise) / LISTEN (big eyes +
//   live mic meter) / THINK (pondering dots) / SING (vowel-mode mouth +
//   loudness squint) / ERROR (dim eyes + zigzag mouth + reason).
// Hard rule: during SING only the mouth sprite (and rare eye pairs) are
//   pushed; a full-screen push (~13ms) can starve the USB RX ring.

#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "wifi_config.h"
#include "face_types.h"

// ---------------------------------------------------------------- link ----
static WiFiClient tcp;
static Stream *link_ = &Serial;
static bool tcpUp = false;
static bool tcpEverUp = false;     // sticky: once TCP wins, serial is debug-only
static bool serialMode = false;    // set 20s after boot if WiFi never joined
static uint32_t lastNetTry = 0;
static IPAddress serverIp;         // cached after first successful resolve
static bool serverIpKnown = false;
static uint32_t lastPoll = 0;
static bool playing = false;       // suppresses POLL + face ticks

static void chirp(int f1, int f2) {           // tiny audio garnish
  M5.Speaker.tone(f1, 40); delay(45);
  if (f2) { M5.Speaker.tone(f2, 60); delay(65); }
}

static void drainStream(Stream *s) {
  uint8_t junk[64];
  while (s->available() > 0) s->readBytes((char *)junk, min(s->available(), 64));
}

static void linkWriteAll(const uint8_t *p, size_t n) {
  // WiFi TCP backpressure can stall writes for a while — be patient (2s)
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

// ---------------------------------------------------------------- face ----
#define C_BG        0x0000
#define C_EYE       0xFFFF
#define C_PUPIL     0x0000
#define C_LIP       0xF800
#define C_MOUTH_IN  0x7800
#define C_TONGUE    0xFB51
#define C_TEETH     0xFFFF
#define C_CHEEK     0xFB51
#define C_LISTEN    0x07FF
#define C_THINK_DIM 0x8410
#define C_REC       0xF800
#define C_ERR       0xFC60
#define C_BATT_WARN 0xFD20

static const char *FACE_NAMES[] = {"BOOT", "IDLE", "LISTEN", "THINK", "SING", "ERROR"};
static FaceState faceState = FACE_BOOT;
static uint32_t faceStateT0 = 0;
static M5Canvas eyeL(&M5.Display), eyeR(&M5.Display), mouth(&M5.Display);
static bool spritesOk = false;

static EyeParams eyeNow = {0, 0, 0, 0};

static void drawEyeInto(M5Canvas &e, const EyeParams &p) {
  e.fillSprite(C_BG);
  int r = (faceState == FACE_LISTEN) ? 15 : 14;
  int pr = (faceState == FACE_LISTEN) ? 7 : 6;
  switch (p.style) {
    case 2:  // happy arc + cheeks
      e.fillCircle(24, 34, 15, C_EYE);
      e.fillCircle(24, 40, 14, C_BG);
      e.fillCircle(8, 43, 4, C_CHEEK);
      e.fillCircle(40, 43, 4, C_CHEEK);
      return;
    case 3:  // dim, half-lidded, looking down (error)
      e.fillCircle(24, 28, 14, 0xC618);
      e.fillCircle(24, 32, 6, C_PUPIL);
      e.fillRect(0, 0, 48, 14 + (55 * 17) / 100, C_BG);
      return;
    case 1:  // squint
      e.fillCircle(24, 28, r, C_EYE);
      e.fillCircle(24, 30, pr, C_PUPIL);
      e.fillRect(0, 0, 48, 24, C_BG);
      return;
    default:
      if (p.lidPct >= 95) { e.fillRoundRect(8, 26, 32, 4, 2, C_EYE); return; }
      e.fillCircle(24, 28, r, C_EYE);
      e.fillCircle(24 + p.pdx, 28 + p.pdy, pr, C_PUPIL);
      if (p.lidPct > 0) e.fillRect(0, 0, 48, 14 + (p.lidPct * 17) / 100, C_BG);
  }
}

static void pushEyes(const EyeParams &p) {
  eyeNow = p;
  if (!spritesOk) return;
  drawEyeInto(eyeL, p); eyeL.pushSprite(51, 6);
  drawEyeInto(eyeR, p); eyeR.pushSprite(141, 6);
}

static void mouthSmile(int dy) {
  if (!spritesOk) return;
  mouth.fillSprite(C_BG);
  mouth.fillCircle(85, 2 + dy, 30, C_LIP);
  mouth.fillCircle(85, -4 + dy, 30, C_BG);
  mouth.pushSprite(35, 55);
}

static void drawNetDot() {
  M5.Display.fillCircle(10, 12, 5, tcpUp ? 0x34DF : C_THINK_DIM);
}

// ---- singing mouth ----
static uint16_t mo100 = 0;
static uint8_t loudAvg = 0, envPrev = 0, squintLvl = 0, modeIdx = 0, mouthMode = 0;
static uint32_t lastEyePush = 0, lastModeSwitch = 0;
static uint16_t lastMouthKey = 0xFFFF;
static const uint8_t modeSeq[8] = {0, 1, 0, 2, 0, 1, 2, 0};

static void drawSingMouth() {
  uint16_t key = mo100 | (mouthMode << 8);
  if (key == lastMouthKey || !spritesOk) return;
  lastMouthKey = key;
  int w, h, r;
  switch (mouthMode) {
    case 1: w = 110 + mo100 * 40 / 100; h = 6 + mo100 * 34 / 100; r = min(h / 2, 8); break;
    case 2: w = 46 + mo100 * 24 / 100; h = 10 + mo100 * 60 / 100; r = min(w / 2, 20); break;
    default: w = 60 + mo100 * 40 / 100; h = 8 + mo100 * 56 / 100; r = min(h / 2, 14);
  }
  if (h > 70) h = 70;
  int x = (170 - w) / 2, y = (72 - h) / 2 + mo100 * 3 / 100;
  mouth.fillSprite(C_BG);
  mouth.fillRoundRect(x, y, w, h, r, C_LIP);
  if (h > 24) mouth.fillRoundRect(x + 6, y + 6, w - 12, h - 12, max(2, r - 4), C_MOUTH_IN);
  if (mouthMode == 1 && h >= 22) mouth.fillRect(x + 8, y + 5, w - 16, 6, C_TEETH);
  if (mo100 > 55) mouth.fillRoundRect(x + w / 2 - 18, y + h - 16, 36, 12, 6, C_TONGUE);
  mouth.pushSprite(35, 55);
}

static void singMouthFrame(uint8_t env) {
  uint16_t t = env * 100u / 255u;
  mo100 = (t > mo100) ? (mo100 * 35 + t * 65) / 100 : (mo100 * 70 + t * 30) / 100;
  if (envPrev < 64 && env >= 96 && millis() - lastModeSwitch > 120) {
    mouthMode = modeSeq[(modeIdx++) & 7];
    lastModeSwitch = millis();
  }
  envPrev = env;
  loudAvg = (loudAvg * 3 + env) / 4;
  uint8_t lvl = squintLvl;
  if (loudAvg >= 190) lvl = 2;
  else if (loudAvg >= 130 && squintLvl < 1) lvl = 1;
  if (squintLvl == 2 && loudAvg < 165) lvl = 1;
  if (squintLvl >= 1 && loudAvg < 105) lvl = 0;
  if (lvl != squintLvl && millis() - lastEyePush >= 160) {
    squintLvl = lvl;
    lastEyePush = millis();
    pushEyes({0, 0, 0, (uint8_t)(lvl == 2 ? 2 : lvl == 1 ? 1 : 0)});
  }
  drawSingMouth();
}

// ---- state machine ----
static uint32_t lastActivity = 0;
static bool sleeping = false;
static uint8_t errorBurst = 0;
static uint32_t errorBurstT0 = 0;

static void faceSetState(FaceState s) {
  faceState = s;
  faceStateT0 = millis();
  lastActivity = millis();
  Serial.printf("dbg face -> %s\n", FACE_NAMES[s]);
  if (sleeping) { sleeping = false; M5.Display.setBrightness(200); }
  M5.Display.fillScreen(C_BG);
  drawNetDot();
  switch (s) {
    case FACE_IDLE:
      pushEyes({0, 0, 0, 0});
      mouthSmile(0);
      break;
    case FACE_LISTEN:
      pushEyes({0, -1, 0, 0});
      if (spritesOk) {
        mouth.fillSprite(C_BG);
        mouth.fillRoundRect(70, 28, 30, 16, 8, C_LIP);
        mouth.fillRoundRect(76, 32, 18, 8, 4, C_MOUTH_IN);
        mouth.pushSprite(35, 55);
      }
      break;
    case FACE_THINK:
      pushEyes({5, -5, 0, 0});
      if (spritesOk) {
        mouth.fillSprite(C_BG);
        mouth.fillRoundRect(65, 33, 40, 6, 3, C_LIP);
        mouth.pushSprite(35, 55);
      }
      break;
    case FACE_SING:
      mo100 = 0; loudAvg = 0; squintLvl = 0; lastMouthKey = 0xFFFF;
      pushEyes({0, 0, 0, 2});   // pre-roll: happy inhale, drawn BEFORE any slurp
      drawSingMouth();
      break;
    default:
      break;
  }
}

static void faceError(const char *reason) {
  Serial.printf("dbg face ERROR: %s\n", reason);
  if (millis() - errorBurstT0 > 10000) { errorBurst = 0; errorBurstT0 = millis(); }
  errorBurst++;
  faceSetState(FACE_ERROR);
  pushEyes({0, 0, 0, 3});
  if (spritesOk) {
    mouth.fillSprite(C_BG);
    for (int dy = 0; dy < 3; dy++) {
      mouth.drawLine(45, 40 + dy, 65, 30 + dy, C_LIP);
      mouth.drawLine(65, 30 + dy, 85, 40 + dy, C_LIP);
      mouth.drawLine(85, 40 + dy, 105, 30 + dy, C_LIP);
      mouth.drawLine(105, 30 + dy, 125, 40 + dy, C_LIP);
    }
    mouth.pushSprite(35, 55);
  }
  M5.Display.setTextDatum(bottom_center);
  M5.Display.setTextColor(C_ERR, C_BG);
  M5.Display.drawString(reason, 120, 133);
}

// idle-tick state
static uint32_t lastFrame = 0, nextBlink = 0, nextWander = 0, nextMicro = 0;
static uint32_t nextBattPoll = 0, lastImu = 0;
static int8_t wtx = 0, wty = 0;
static int8_t blinkFrame = -1;
static uint8_t breathIdx = 0;
static uint32_t lastBreath = 0;
static const int8_t breathTab[6] = {0, 1, 1, 0, -1, -1};
static const int8_t wanderTab[7][2] = {{0,0},{4,0},{-4,0},{3,-2},{-3,-2},{2,2},{0,-3}};

static void faceTick() {
  uint32_t now = millis();
  if (faceState == FACE_SING || faceState == FACE_LISTEN) return;
  if (now - lastFrame < 50) return;
  lastFrame = now;

  if (faceState == FACE_BOOT) {
    uint32_t el = now - faceStateT0;
    int frame = el / 50;
    if (frame <= 8) {
      int r = 2 + frame * 12 / 8;
      if (spritesOk) {
        eyeL.fillSprite(C_BG); eyeL.fillCircle(24, 28, r, C_EYE);
        eyeL.fillCircle(24, 28, max(1, r * 6 / 14), C_PUPIL); eyeL.pushSprite(51, 6);
        eyeR.fillSprite(C_BG); eyeR.fillCircle(24, 28, r, C_EYE);
        eyeR.fillCircle(24, 28, max(1, r * 6 / 14), C_PUPIL); eyeR.pushSprite(141, 6);
      }
      if (frame == 4) {
        mouthSmile(0);
        M5.Display.setTextDatum(bottom_center);
        M5.Display.setTextColor(C_THINK_DIM, C_BG);
        M5.Display.drawString("mikkey", 120, 133);
      }
    } else if (frame >= 9 && frame <= 13) {
      static const uint8_t lids[5] = {60, 100, 100, 40, 0};
      pushEyes({0, 0, lids[frame - 9], 0});
    }
    if (el > 900) faceSetState(FACE_IDLE);
    return;
  }

  if (faceState == FACE_ERROR) {
    if (errorBurst >= 3) {   // persistent trouble: stay, slow-blink until next event
      if (((now / 1000) & 1) == 0) pushEyes({0, 0, 55, 3});
      else pushEyes({0, 0, 80, 3});
      return;
    }
    if (now - faceStateT0 > 2500) faceSetState(FACE_IDLE);
    return;
  }

  if (faceState == FACE_THINK) {
    int active = (now / 350) % 3;
    static const int dx[3] = {106, 120, 134};
    for (int i = 0; i < 3; i++) {
      M5.Display.fillCircle(dx[i], 112, 6, C_BG);
      M5.Display.fillCircle(dx[i], 112, i == active ? 5 : 3, i == active ? C_EYE : C_THINK_DIM);
    }
    if (((now - faceStateT0) / 1600) & 1) { if (eyeNow.pdx != -5) pushEyes({-5, -5, 0, 0}); }
    else { if (eyeNow.pdx != 5) pushEyes({5, -5, 0, 0}); }
    if (now - faceStateT0 > 15000) { Serial.println("dbg face think timeout"); faceSetState(FACE_IDLE); }
    return;
  }

  // ---- FACE_IDLE ----
  // IMU: pickup / shake surprise (also wakes from sleep)
  if (now - lastImu > 120) {
    lastImu = now;
    float ax, ay, az;
    if (M5.Imu.getAccel(&ax, &ay, &az)) {
      float mag = sqrtf(ax * ax + ay * ay + az * az);
      static uint32_t lastSurprise = 0;
      if (fabsf(mag - 1.0f) > 0.35f && now - lastSurprise > 3000) {
        lastSurprise = now;
        lastActivity = now;
        if (sleeping) { sleeping = false; M5.Display.setBrightness(200); }
        Serial.println("dbg imu surprise");
        pushEyes({0, 0, 0, 0});
        if (spritesOk) {  // pupil-dot wide eyes
          eyeL.fillSprite(C_BG); eyeL.fillCircle(24, 28, 15, C_EYE);
          eyeL.fillCircle(24, 28, 3, C_PUPIL); eyeL.pushSprite(51, 6);
          eyeR.fillSprite(C_BG); eyeR.fillCircle(24, 28, 15, C_EYE);
          eyeR.fillCircle(24, 28, 3, C_PUPIL); eyeR.pushSprite(141, 6);
        }
        chirp(880, 1320);
        nextWander = now + 900;   // hold the surprise briefly
        return;
      }
    }
  }

  // sleep after 45s of nothing
  if (!sleeping && now - lastActivity > 45000) {
    sleeping = true;
    Serial.println("dbg face sleeping");
    pushEyes({0, 0, 100, 0});
    M5.Display.setTextDatum(bottom_center);
    M5.Display.setTextColor(C_THINK_DIM, C_BG);
    M5.Display.drawString("z z Z", 190, 30);
  }
  if (sleeping) {
    // breathing backlight, table-driven
    static const uint8_t br[8] = {60, 80, 105, 120, 105, 80, 60, 50};
    M5.Display.setBrightness(br[(now / 400) & 7]);
    return;
  }

  // blink
  if (blinkFrame >= 0) {
    static const uint8_t lids[5] = {60, 100, 100, 40, 0};
    pushEyes({wtx, wty, lids[blinkFrame], 0});
    if (++blinkFrame > 4) {
      blinkFrame = -1;
      if (esp_random() % 100 < 15) nextBlink = now + 180;   // double blink
      else nextBlink = now + 2800 + esp_random() % 3700;
    }
    return;
  }
  if (now >= nextBlink) { blinkFrame = 0; return; }

  // pupil wander
  if (now >= nextWander) {
    const int8_t *t = wanderTab[esp_random() % 7];
    wtx = t[0]; wty = t[1];
    nextWander = now + 1200 + esp_random() % 2300;
  }
  int8_t ndx = eyeNow.pdx + (wtx - eyeNow.pdx) * 3 / 10;
  int8_t ndy = eyeNow.pdy + (wty - eyeNow.pdy) * 3 / 10;
  if (abs(wtx - eyeNow.pdx) <= 1) ndx = wtx;
  if (abs(wty - eyeNow.pdy) <= 1) ndy = wty;
  if (ndx != eyeNow.pdx || ndy != eyeNow.pdy) pushEyes({ndx, ndy, 0, 0});

  // breathing smile
  if (now - lastBreath > 500) {
    lastBreath = now;
    breathIdx = (breathIdx + 1) % 6;
    mouthSmile(breathTab[breathIdx]);
  }

  // battery glyph + report
  if (now >= nextBattPoll) {
    nextBattPoll = now + 30000;
    int lvl = M5.Power.getBatteryLevel();
    Serial.printf("dbg batt %d\n", lvl);
    M5.Display.fillRect(211, 5, 20, 10, C_BG);
    if (lvl >= 0 && lvl < 20) {
      M5.Display.drawRect(212, 6, 16, 8, C_EYE);
      M5.Display.fillRect(228, 8, 2, 4, C_EYE);
      M5.Display.fillRect(214, 8, max(1, lvl * 12 / 100), 4, lvl < 10 ? C_REC : C_BATT_WARN);
    }
  }
}

// -------------------------------------------------------------- protocol ----
static const uint32_t CHUNK = 441;
static const size_t ENV_MAX = 16384;
static const size_t PCM_MAX = 4 * 1024 * 1024;   // caps clips at ~95s @22k
static uint8_t envBuf[ENV_MAX];
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
  while (remaining > 0) {
    uint32_t take = remaining < CHUNK ? remaining : CHUNK;
    while (M5.Speaker.isPlaying(0) >= 2) { pollButton(); delay(1); }
    if (framesDone > 0 && M5.Speaker.isPlaying(0) == 0) underruns++;
    M5.Speaker.playRaw(clipBuf + framesDone * CHUNK, take, rate, false, 1, 0);
    if (framesDone < nEnv) singMouthFrame(envBuf[framesDone]);
    framesDone++;
    remaining -= take;
  }
  while (M5.Speaker.isPlaying(0)) { pollButton(); delay(5); }
  playing = false;
  mo100 = 0; drawSingMouth();
  if (underruns) link_->printf("UNDERRUN %lu\n", (unsigned long)underruns);
  link_->printf("DONE %lu\n", (unsigned long)framesDone);
  pushEyes({0, 0, 0, 2});      // finish flourish
  delay(350);
  faceSetState(FACE_IDLE);
}

static const uint32_t PREBUFFER_CELLS = 25;   // 0.5s

static void playClipStream(uint32_t rate) {
  Serial.printf("dbg stream start @ %lu Hz\n", (unsigned long)rate);
  if (!clipBuf) { faceError("no buffer"); link_->println("DONE 0"); return; }
  playing = true;
  uint32_t cells = 0, played = 0, underruns = 0, badTags = 0;
  bool done = false, started = false;
  uint32_t lastData = millis();

  while (true) {
    // drain everything waiting first — dropped bytes = permanent desync (USB)
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
      if (played > 0 && M5.Speaker.isPlaying(0) == 0) underruns++;
      M5.Speaker.playRaw(clipBuf + (size_t)played * 441, 441, rate, false, 1, 0);
      if ((played & 1) == 0) singMouthFrame(envBuf[played]);   // 25fps mouth
      played++;
    }
    if (done && played >= cells && !M5.Speaker.isPlaying(0)) break;
    if (done && cells == 0) break;
    pollButton();
    delay(1);
  }
  playing = false;
  mo100 = 0; drawSingMouth();
  if (badTags > 8) { faceError("bad tags"); }
  if (underruns) link_->printf("UNDERRUN %lu\n", (unsigned long)underruns);
  link_->printf("DONE %lu\n", (unsigned long)played);
  if (badTags <= 8) {
    pushEyes({0, 0, 0, 2});
    delay(350);
    faceSetState(FACE_IDLE);
  }
}

// hold BtnA = push-to-talk
static const uint32_t MIC_RATE = 16000;
static const size_t MIC_CHUNK = 512;
static uint16_t meterH[3] = {0, 0, 0};
static uint32_t lastMeter = 0;

static void faceListenTick(const int16_t *buf, size_t n) {
  uint32_t now = millis();
  if (now - lastMeter < 64) return;
  lastMeter = now;
  M5.Display.fillCircle(224, 14, 9, C_BG);
  M5.Display.fillCircle(224, 14, ((now / 500) & 1) ? 8 : 6, C_REC);
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i += 4) acc += abs(buf[i]);
  uint16_t h = min((uint32_t)24, (acc / (n / 4)) / 120);
  meterH[2] = meterH[1]; meterH[1] = meterH[0]; meterH[0] = h;
  static const int mx[3] = {128, 116, 104};
  for (int i = 0; i < 3; i++) {
    M5.Display.fillRect(mx[i], 102, 8, 24, C_BG);
    M5.Display.fillRect(mx[i], 126 - meterH[i], 8, max((int)meterH[i], 2), C_LISTEN);
  }
}

static void recordAndSend() {
  if (!clipBuf) { faceError("no buffer"); return; }
  faceSetState(FACE_LISTEN);
  link_->println("REC");
  M5.Speaker.setVolume(0); delay(15);    // soften the codec pop
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
  // discard anything that raced in while we weren't reading (partial pushes)
  delay(30);
  drainStream(link_);
  faceSetState(FACE_THINK);
}

// ------------------------------------------------------------------ net ----
static void tryNet() {
  if (playing || faceState == FACE_LISTEN) return;
  if (link_->available() > 0) return;          // never stall while data waits
  if (WiFi.status() != WL_CONNECTED) {
    if (tcpUp) { tcpUp = false; drawNetDot(); if (!tcpEverUp) link_ = &Serial; }
    if (millis() - lastNetTry > 8000) {
      lastNetTry = millis();
      Serial.println("dbg wifi connecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (tcp.connected()) {
    if (!tcpUp) {
      tcpUp = true;
      if (!tcpEverUp) chirp(900, 1400);
      tcpEverUp = true;
      link_ = &tcp;
      drawNetDot();
    }
    return;
  }
  if (tcpUp) {
    tcpUp = false;
    drawNetDot();
    chirp(1400, 900);
    Serial.println("dbg tcp lost");
    // sticky: do NOT fall back to serial protocol once TCP has ever owned
    // the link — dynamic handoff is how clips get lost. Keep reconnecting.
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
      link_ = &tcp;
      drainStream(&Serial);        // abandon anything stale on the USB ring
      drawNetDot();
      link_->printf("READY tcp rssi=%d bat=%d\n", WiFi.RSSI(), M5.Power.getBatteryLevel());
      Serial.println("dbg tcp connected");
    } else {
      serverIpKnown = false;       // re-resolve next attempt
    }
  }
}

// ---------------------------------------------------------------- setup ----
static const uint8_t MAGIC[4] = {'M', 'I', 'K', 'Y'};

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(255);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(200);

  spritesOk = true;
  eyeL.setColorDepth(16);
  eyeR.setColorDepth(16);
  mouth.setColorDepth(16);
  if (!eyeL.createSprite(48, 48) || !eyeR.createSprite(48, 48) ||
      !mouth.createSprite(170, 72)) {
    spritesOk = false;
  }

  clipBuf = (int16_t *)ps_malloc(PCM_MAX);

  Serial.setRxBufferSize(32768);
  Serial.begin(115200);
  delay(300);
  if (!clipBuf) Serial.println("dbg FATAL: ps_malloc failed");
  if (!spritesOk) Serial.println("dbg FATAL: face sprite alloc failed");
  Serial.printf("READY board=%d psram=%u\n", (int)M5.getBoard(), ESP.getPsramSize());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  MDNS.begin("mikkey");

  faceSetState(FACE_BOOT);
  // boot arpeggio
  M5.Speaker.tone(660, 70); delay(80);
  M5.Speaker.tone(880, 70); delay(80);
  M5.Speaker.tone(1320, 110);
}

void loop() {
  pollButton();
  faceTick();
  if (M5.BtnA.wasHold()) { recordAndSend(); return; }
  tryNet();

  // POLL: the server only pushes in response to this, so a push is always
  // bound to the link we are actually reading. Suppressed while busy.
  if (!playing && faceState != FACE_LISTEN && !M5.BtnA.isPressed() &&
      millis() - lastPoll > 500) {
    lastPoll = millis();
    link_->printf("POLL bat=%d rssi=%d\n", M5.Power.getBatteryLevel(),
                  tcpUp ? WiFi.RSSI() : 0);
  }

  // drain-discard the inactive transport so stale pushes can't desync us later
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
