// Mikkey — pop-star face: 1-bit pixel line art, black on white, in the
// spirit of minimalist MJ pixel portraits. Portrait 135x240 = 45x80 grid x3.
// Hair locks hang from under the physical hat; heavy-lidded eyes with lashes,
// arched brows, pupils that glance, a mouth that opens in three steps while
// singing, and "the lean" silhouette after a song. Art lives in
// tools/mj_art.py (ASCII) -> mj_art.h. Same five hooks as the other faces.
#pragma once
#include <M5Unified.h>
#include "face_types.h"
#include "mj_art.h"

static const int W = 135, H = 240, SC = 3;
static const uint16_t C_INK = 0x1082, C_PAPER = 0xFFFF;
static const int MOUTH_Y = MJ_MOUTH_R * SC, MOUTH_H = 9 * SC + 6;      // mouth strip
static const int EYES_Y = MJ_EYEL_R * SC, EYES_H = 16 * SC;            // eyes strip

static M5Canvas scene(&M5.Display);      // 135x240 (PSRAM)
static M5Canvas mouthCv(&M5.Display);    // mouth strip (DRAM, speed-critical)
static M5Canvas eyesCv(&M5.Display);     // eyes strip (DRAM)
static bool dSpritesOk = false;

static FaceState faceState = FACE_BOOT;
static uint32_t faceStateT0 = 0;
static bool dragonNetUp = false;
static const char *FACE_NAMES2[] = {"BOOT", "IDLE", "LISTEN", "THINK", "SING", "ERROR"};

enum { MOOD_PLAIN = 0, MOOD_UPBEAT, MOOD_SAD, MOOD_SLEEPY };
static uint8_t dragonMood = MOOD_PLAIN;
static void dragonSetMood(uint8_t m) { dragonMood = m; Serial.printf("dbg mood=%d\n", m); }

// draw a 1-bit sprite at grid (col,row); oy = canvas y offset in px
static void drawB(M5Canvas &c, const BSprite &s, int col, int row, bool flip, int oy = 0) {
  for (int y = 0; y < s.h; y++) {
    const char *r = s.rows[y];
    for (int x = 0; x < s.w; x++)
      if (r[x] == '#')
        c.fillRect((col + (flip ? s.w - 1 - x : x)) * SC, (row + y) * SC - oy, SC, SC, C_INK);
  }
}
static void drawIris(M5Canvas &c, int col, int row, int dx, int dy, int oy = 0) {
  int cx = col + dx, cy = row + dy;
  c.fillRect((cx - 2) * SC, (cy - 2) * SC - oy, 4 * SC, SC, C_INK);
  c.fillRect((cx - 3) * SC, (cy - 1) * SC - oy, 6 * SC, 3 * SC, C_INK);
  c.fillRect((cx - 2) * SC, (cy + 2) * SC - oy, 4 * SC, SC, C_INK);
  c.fillRect((cx - 2) * SC, (cy - 1) * SC - oy, SC, SC, C_PAPER);   // highlight
}

enum { EYE_OPEN, EYE_BLINK, EYE_HALF, EYE_CLOSED, EYE_HAPPY, EYE_SQUINT, EYE_SAD, EYE_ANGRY };
enum { MOUTH_CLOSED, MOUTH_HALF, MOUTH_WIDE, MOUTH_SMILE };

static const BSprite &eyeSpr(uint8_t e) {
  switch (e) {
    case EYE_HALF:   return MJ_EYE_HALF;
    case EYE_BLINK: case EYE_CLOSED: return MJ_EYE_CLOSED;
    case EYE_HAPPY: case EYE_SQUINT: return MJ_EYE_HAPPY;
    case EYE_SAD:    return MJ_EYE_SAD;
    case EYE_ANGRY:  return MJ_EYE_ANGRY;
    default:         return MJ_EYE_OPEN;
  }
}
static const BSprite &mouthSpr(uint8_t m) {
  switch (m) {
    case MOUTH_HALF:  return MJ_MOUTH_HALF;
    case MOUTH_WIDE:  return MJ_MOUTH_WIDE;
    case MOUTH_SMILE: return MJ_MOUTH_SMILE;
    default:          return MJ_MOUTH_CLOSED;
  }
}

// eyes: iris first, lids over it (a half lid hides the top of the iris)
static void drawEyes(M5Canvas &c, uint8_t e, int pdx, int pdy, int oy = 0) {
  bool iris = (e == EYE_OPEN || e == EYE_HALF || e == EYE_SAD || e == EYE_ANGRY);
  if (iris) {
    int dy = pdy + (e == EYE_HALF ? 2 : 0);
    drawIris(c, MJ_PUPILL_C, MJ_PUPILL_R, pdx, dy, oy);
    drawIris(c, MJ_PUPILR_C, MJ_PUPILR_R, pdx, dy, oy);
    if (e == EYE_HALF) c.fillRect(0, MJ_EYEL_R * SC - oy, W, 8 * SC, C_PAPER);
  }
  drawB(c, eyeSpr(e), MJ_EYEL_C, MJ_EYEL_R, false, oy);
  drawB(c, eyeSpr(e), MJ_EYER_C, MJ_EYER_R, true, oy);
}
static void drawMouth(M5Canvas &c, uint8_t m, int oy = 0) {
  drawB(c, mouthSpr(m), MJ_MOUTH_C, MJ_MOUTH_R, false, oy);
}
static void drawStatic(M5Canvas &c) {
  c.fillSprite(C_PAPER);
  drawB(c, MJ_HAIR, 0, 0, false);
  drawB(c, MJ_NOSE, MJ_NOSE_C, MJ_NOSE_R, false);
}

// ---------------------------------------------------------------- life ----
enum Life { L_IDLE, L_SLEEPY, L_ASLEEP, L_WAKE, L_STARTLE, L_DIZZY, L_FLOURISH, L_GLANCE };
static uint8_t life = L_IDLE;
static uint32_t lifeT0 = 0, lifeDwell = 0;
static uint32_t lastActivity = 0, lastProtoEvent = 0;
static uint32_t nextBlink = 0, blinkUntil = 0, nextGlance = 0;
static int glanceDx = 0;
static char errReason[20] = "";
static uint8_t flourishMood = MOOD_PLAIN;
static bool wasSing = false, wasErr = false;
static void lifeEnter(uint8_t s, uint32_t dwell) { life = s; lifeT0 = millis(); lifeDwell = dwell; }

// ------------------------------------------------------------------ imu ----
static int32_t gfx = 0, gfy = 0, gfz = 1000;
static uint8_t shakeHits = 0, startleCd = 0, gyroRun = 0;
static bool wantStartle = false, wantDizzy = false;

static void physSample() {
  float fx, fy, fz;
  if (M5.Imu.getAccel(&fx, &fy, &fz)) {
    int32_t rx = fx * 1000, ry = fy * 1000, rz = fz * 1000;
    gfx += (rx - gfx) >> 3; gfy += (ry - gfy) >> 3; gfz += (rz - gfz) >> 3;
    int32_t hp = abs(rx - gfx) + abs(ry - gfy) + abs(rz - gfz);
    if (hp > 500) shakeHits = min(shakeHits + 2, 12);
    else if (shakeHits) shakeHits--;
    if (shakeHits >= 6 && !startleCd) { wantStartle = true; shakeHits = 0; startleCd = 30; }
    if (startleCd) startleCd--;
  }
  float gx, gy, gz;
  if (M5.Imu.getGyro(&gx, &gy, &gz)) {
    int32_t g = abs((int)gx) + abs((int)gy) + abs((int)gz);
    if (g > 500) { if (++gyroRun >= 2) wantDizzy = true; }
    else gyroRun = 0;
  }
}
static bool lowBattery() {
  static int batt = 100;
  static uint32_t nextPoll = 0;
  if (millis() > nextPoll) { nextPoll = millis() + 30000; batt = M5.Power.getBatteryLevel(); }
  return batt >= 0 && batt < 20;
}

// ------------------------------------------------------- protocol hooks ----
static uint8_t jaw = 0, wideRun = 0;
static int8_t lastMouth = -1, lastSquint = -1;
static uint32_t lastFrame = 0, lastListenDraw = 0;
static uint16_t eqH[3] = {0, 0, 0};

static void drawStrip(M5Canvas &c) {
  c.fillCircle(8, H - 8, 3, dragonNetUp ? 0x34DF : 0xC618);
  if (lowBattery()) {
    c.drawRect(W - 22, H - 12, 14, 7, C_INK);
    c.fillRect(W - 8, H - 10, 2, 3, C_INK);
  }
}

static void drawSingBackdrop() {
  drawStatic(scene);
  drawEyes(scene, EYE_OPEN, 0, 0);
  drawMouth(scene, MOUTH_CLOSED);
  if (dragonMood == MOOD_SLEEPY) { scene.setTextDatum(top_right); scene.setTextColor(C_INK, C_PAPER); scene.drawString("z z", W - 4, 36); }
  drawStrip(scene);
  scene.pushSprite(0, 0);
}

static void faceSetState(FaceState s) {
  if (faceState == FACE_SING && s == FACE_IDLE) wasSing = true;
  if (s == FACE_LISTEN || s == FACE_THINK || s == FACE_SING || s == FACE_ERROR)
    lastProtoEvent = millis();
  faceState = s;
  faceStateT0 = millis();
  lastActivity = millis();
  Serial.printf("dbg face -> %s\n", FACE_NAMES2[s]);
  M5.Display.setBrightness(200);
  if (s == FACE_IDLE) {
    if (wasSing) {
      wasSing = false;
      flourishMood = dragonMood;
      dragonMood = MOOD_PLAIN;
      lifeEnter(L_FLOURISH, flourishMood == MOOD_SAD ? 2200 : 4000);
    } else if (wasErr) {
      wasErr = false;
      lifeEnter(L_DIZZY, 2000);
    } else {
      lifeEnter(L_IDLE, 0);
    }
  } else if (s == FACE_SING) {
    M5.Speaker.stop();                           // audio guard: clean slate
    jaw = 0; wideRun = 0; lastMouth = -1; lastSquint = -1;
    if (dSpritesOk) drawSingBackdrop();          // once, pre-slurp
  }
}

static void faceError(const char *reason) {
  Serial.printf("dbg face ERROR: %s\n", reason);
  strncpy(errReason, reason, sizeof(errReason) - 1);
  wasErr = true;
  faceSetState(FACE_ERROR);
}

static void singMouthFrame(uint8_t env) {
  if (!dSpritesOk) return;
  uint8_t target = env;
  if (target > jaw) jaw += (target - jaw) >> 1;
  else jaw -= (jaw - target) >> 2;
  int8_t m;
  if (jaw < 64) { m = MOUTH_CLOSED; wideRun = 0; }
  else if (jaw < 160) { m = MOUTH_HALF; wideRun = 0; }
  else { m = MOUTH_WIDE; wideRun = (uint8_t)min((int)wideRun + 1, 30); }
  int8_t sq = (m == MOUTH_WIDE && wideRun >= 8) ? 1 : 0;   // big note: eyes close happily
  if (m != lastMouth) {
    lastMouth = m;
    mouthCv.fillSprite(C_PAPER);
    drawMouth(mouthCv, m, MOUTH_Y);
    mouthCv.pushSprite(0, MOUTH_Y);
  }
  if (sq != lastSquint) {
    lastSquint = sq;
    eyesCv.fillSprite(C_PAPER);
    drawEyes(eyesCv, sq ? EYE_HAPPY : EYE_OPEN, 0, 0, EYES_Y);
    eyesCv.pushSprite(0, EYES_Y);
  }
}

static void faceListenTick(const int16_t *buf, size_t n) {
  uint32_t now = millis();
  if (now - lastListenDraw < 66 || !dSpritesOk) return;
  lastListenDraw = now;
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i += 4) acc += abs(buf[i]);
  uint16_t h = min((uint32_t)30, (acc / (n / 4)) / 120);
  eqH[2] = eqH[1]; eqH[1] = eqH[0]; eqH[0] = h;
  drawStatic(scene);
  drawEyes(scene, EYE_OPEN, 0, 0);
  drawMouth(scene, MOUTH_CLOSED);
  for (int i = 0; i < 3; i++)                        // listening meter under the chin
    scene.fillRect(52 + i * 12, H - 10 - eqH[i], 8, max((int)eqH[i], 3), C_INK);
  scene.fillCircle(W - 12, 12, ((now / 400) & 1) ? 6 : 4, 0xF800);
  drawStrip(scene);
  scene.pushSprite(0, 0);
}

static void renderScene() {
  uint32_t now = millis();
  uint32_t el = now - lifeT0;
  uint8_t mouth = MOUTH_CLOSED, eyes = EYE_OPEN;
  int pdx = 0, pdy = 0;

  if (life == L_FLOURISH && flourishMood != MOOD_SAD && el < 2200) {
    // THE LEAN — silhouette on white, then back to the face
    scene.fillSprite(C_PAPER);
    drawB(scene, MJ_LEAN, MJ_LEAN_C + (el < 300 ? 0 : 0), MJ_LEAN_R, false);
    if (el < 80) M5.Speaker.tone(660, 50);
    drawStrip(scene);
    scene.pushSprite(0, 0);
    return;
  }
  drawStatic(scene);

  if (faceState == FACE_ERROR || life == L_DIZZY) {
    mouth = MOUTH_HALF; eyes = EYE_ANGRY;
    for (int i = 0; i < 3; i++) {
      int a = (now / 100 + i * 120) % 360;
      int sx = W / 2 + ((a < 180 ? a : 360 - a) - 90) * 60 / 90;
      scene.fillRect(sx, 36 + i * 6, 5, 5, C_INK);
    }
    if (faceState == FACE_ERROR) {
      scene.setTextDatum(bottom_center);
      scene.setTextColor(C_INK, C_PAPER);
      scene.drawString(errReason, W / 2, H - 2);
    }
  } else if (faceState == FACE_THINK) {
    uint32_t tel = now - faceStateT0;
    pdx = 2; pdy = -1;                              // eyes up and away
    mouth = ((tel / 700) & 1) ? MOUTH_HALF : MOUTH_CLOSED;
    if (tel >= 1100) {
      static const int freqs[3] = {523, 659, 784};
      int i = (int)((tel - 1100) / 200);
      if (i < 3 && ((tel - 1100) % 200) < 60) M5.Speaker.tone(freqs[i], 70);
    }
    scene.setTextDatum(bottom_center);
    scene.setTextColor(C_INK, C_PAPER);
    scene.drawString(((now / 800) & 1) ? "?" : "...", W / 2, H - 2);
  } else if (life == L_ASLEEP) {
    eyes = EYE_CLOSED;
    if ((now / 900) & 1) { scene.setTextDatum(top_right); scene.setTextColor(C_INK, C_PAPER); scene.drawString("z z", W - 4, 36); }
  } else if (life == L_SLEEPY) {
    eyes = EYE_HALF;
    if ((now / 3000) % 3 == 0) { scene.setTextDatum(top_right); scene.setTextColor(C_INK, C_PAPER); scene.drawString("z", W - 4, 36); }
  } else if (life == L_WAKE) {
    if (el < 400) eyes = EYE_CLOSED;
    else if (el < 800) eyes = EYE_HALF;
    else if (el < 1400) { eyes = EYE_CLOSED; mouth = MOUTH_WIDE; }   // yawn
    else eyes = EYE_OPEN;
  } else if (life == L_STARTLE) {
    mouth = MOUTH_WIDE; eyes = EYE_OPEN;
    scene.setTextDatum(top_right);
    scene.setTextColor(C_INK, C_PAPER);
    scene.drawString("!", W - 4, 36);
  } else if (life == L_FLOURISH) {
    if (flourishMood == MOOD_SAD) { eyes = EYE_SAD; }
    else { eyes = EYE_HAPPY; mouth = MOUTH_SMILE; }
  } else {
    if (now >= nextBlink) { blinkUntil = now + 130; nextBlink = now + 2500 + esp_random() % 3000; }
    if (now < blinkUntil) eyes = EYE_BLINK;
    if (life == L_GLANCE) pdx = glanceDx;
    if (dragonMood == MOOD_SAD && now >= blinkUntil) eyes = EYE_SAD;
  }

  drawEyes(scene, eyes, pdx, pdy);
  drawMouth(scene, mouth);
  drawStrip(scene);
  scene.pushSprite(0, 0);
}

static void faceTick() {
  uint32_t now = millis();
  if (faceState == FACE_SING || faceState == FACE_LISTEN) return;
  if (now - lastFrame < ((life == L_ASLEEP) ? 200 : 50)) return;
  lastFrame = now;

  if (faceState == FACE_BOOT) {
    if (now - faceStateT0 > 400) faceSetState(FACE_IDLE);
    return;
  }
  physSample();
  if (faceState == FACE_THINK) {
    renderScene();
    if (now - faceStateT0 > 15000) { Serial.println("dbg think timeout"); faceSetState(FACE_IDLE); }
    return;
  }
  if (faceState == FACE_ERROR) {
    renderScene();
    if (now - faceStateT0 > 2500) { wasErr = false; faceSetState(FACE_IDLE); }
    return;
  }

  if (wantStartle) {
    wantStartle = false;
    lastActivity = now;
    if (life == L_ASLEEP || life == L_SLEEPY) {
      M5.Display.setBrightness(200);
      lifeEnter(L_WAKE, 1800);
    } else {
      M5.Speaker.tone(880, 40);
      lifeEnter(L_STARTLE, 900);
    }
  }
  if (wantDizzy) {
    wantDizzy = false;
    lastActivity = now;
    if (life != L_ASLEEP && life != L_WAKE) lifeEnter(L_DIZZY, 1600);
  }

  switch (life) {
    case L_IDLE:
      if (now - lastActivity > 45000) { lifeEnter(L_SLEEPY, 0); break; }
      if (now >= nextGlance) {
        glanceDx = (esp_random() & 1) ? 2 : -2;
        nextGlance = now + 4000 + esp_random() % 5000;
        lifeEnter(L_GLANCE, 500 + esp_random() % 500);
      }
      break;
    case L_GLANCE:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_IDLE, 0);
      break;
    case L_SLEEPY:
      if (now - lastActivity > 105000) { lifeEnter(L_ASLEEP, 0); M5.Display.setBrightness(90); }
      break;
    case L_ASLEEP: {
      static const uint8_t br[8] = {60, 80, 105, 120, 105, 80, 60, 50};
      M5.Display.setBrightness(br[(now / 400) & 7]);
      break;
    }
    default:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_IDLE, 0);
  }
  renderScene();
}

static void dragonInit() {
  scene.setColorDepth(16);
  scene.setPsram(true);
  mouthCv.setColorDepth(16);
  eyesCv.setColorDepth(16);
  dSpritesOk = scene.createSprite(W, H) && mouthCv.createSprite(W, MOUTH_H) && eyesCv.createSprite(W, EYES_H);
  if (!dSpritesOk) Serial.println("dbg FATAL: mjface sprite alloc failed");
  lastActivity = lastProtoEvent = millis();
  nextBlink = millis() + 1500;
  nextGlance = millis() + 4000;
  lifeEnter(L_WAKE, 1800);
}
