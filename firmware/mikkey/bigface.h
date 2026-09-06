// Mikkey — big-face presentation for the 3D-printed hat.
// Portrait screen (135x240). The head is drawn WITHOUT its pixel fedora, top
// flush with the hat end of the stick, so the physical hat sits on his head
// in every state: idle, listen, think, sing, error. Body below, status strip
// at the bottom. Same five hooks the protocol code calls:
//   faceSetState / faceTick / singMouthFrame / faceListenTick / faceError
// plus dragonInit / dragonSetMood / dragonNetUp for source compatibility.
#pragma once
#include <M5Unified.h>
#include "face_types.h"
#include "dragon_art.h"

// ------------------------------------------------------------- layout ----
static const int W = 135, H = 240, SC = 9;
static const int HEAD_ROW0 = 2, HEAD_ROW1 = 15, HEAD_COL0 = 5;   // SING_* minus the hat rows
static const int HEAD_X = 4, HEAD_Y = 0, HEAD_W = 14 * SC, HEAD_H = 14 * SC;  // 126x126
static const int BODY_ROW0 = 9, BODY_ROW1 = 17, BODY_COL0 = 7;   // IDLE_* body + feet
static const int BODY_X = 18, BODY_Y = HEAD_Y + HEAD_H;          // 126..207
static const int STRIP_Y = 208;                                  // status strip
static const uint16_t C_SKIN = 0xF694, C_DARK = 0x2104, C_WHITE = 0xFFFF;

static M5Canvas scene(&M5.Display);      // 135x240 (PSRAM)
static M5Canvas singHead(&M5.Display);   // 126x126 close-up (DRAM, speed-critical)
static bool dSpritesOk = false;

static FaceState faceState = FACE_BOOT;
static uint32_t faceStateT0 = 0;
static bool dragonNetUp = false;
static const char *FACE_NAMES2[] = {"BOOT", "IDLE", "LISTEN", "THINK", "SING", "ERROR"};

enum { MOOD_PLAIN = 0, MOOD_UPBEAT, MOOD_SAD, MOOD_SLEEPY };
static uint8_t dragonMood = MOOD_PLAIN;
static void dragonSetMood(uint8_t m) { dragonMood = m; Serial.printf("dbg mood=%d\n", m); }

static void drawRows(M5Canvas &c, const DSprite &s, int x, int y, int sc,
                     int row0, int row1, int col0) {
  for (int cy = row0; cy <= row1 && cy < s.h; cy++)
    for (int cx = 0; cx < s.w; cx++) {
      uint8_t v = s.px[cy * s.w + cx];
      if (!v) continue;
      c.fillRect(x + (cx - col0) * sc, y + (cy - row0) * sc, sc, sc, DPAL[v]);
    }
}
static void drawSpr(M5Canvas &c, const DSprite &s, int x, int y, int sc) {
  drawRows(c, s, x, y, sc, 0, s.h - 1, 0);
}

// ---------------------------------------------------------------- eyes ----
// The eyes are 3x3 blocks at sprite rows 4..6, cols 7..9 and 14..16.
// Procedural so the big face can blink, glance, doze and squint.
enum { EYE_OPEN, EYE_BLINK, EYE_HALF, EYE_CLOSED, EYE_HAPPY, EYE_SQUINT, EYE_SAD };

static void drawEyes(M5Canvas &c, uint8_t style, int pdx, int pdy) {
  for (int e = 0; e < 2; e++) {
    int col = e ? 14 : 7;
    int x0 = HEAD_X + (col - HEAD_COL0) * SC, y0 = HEAD_Y + (4 - HEAD_ROW0) * SC;
    for (int r = 0; r < 3; r++)
      for (int k = 0; k < 3; k++) {
        uint16_t col16 = C_WHITE;
        switch (style) {
          case EYE_OPEN:   col16 = (r == 1 + pdy && k == 1 + pdx) ? C_DARK : C_WHITE; break;
          case EYE_BLINK:
          case EYE_CLOSED: col16 = (r == 1) ? C_DARK : C_SKIN; break;
          case EYE_HALF:   col16 = (r == 0) ? C_SKIN : (r == 1 && k == 1) ? C_DARK : C_WHITE; break;
          case EYE_HAPPY:  col16 = ((r == 0 && k == 1) || (r == 1 && k != 1)) ? C_DARK : C_SKIN; break;
          case EYE_SQUINT: col16 = ((r == 0 && k != 1) || (r == 1 && k == 1) || (r == 2 && k != 1)) ? C_DARK : C_SKIN; break;
          case EYE_SAD:    col16 = (r == 0) ? C_SKIN : (r == 1 && k == 1) ? C_DARK
                                 : (r == 1 && ((e == 0 && k == 0) || (e == 1 && k == 2))) ? C_SKIN : C_WHITE; break;
        }
        c.fillRect(x0 + k * SC, y0 + r * SC, SC, SC, col16);
      }
  }
}

static void drawHead(M5Canvas &c, const DSprite &s) {
  drawRows(c, s, HEAD_X, HEAD_Y, SC, HEAD_ROW0, HEAD_ROW1, HEAD_COL0);
}
static void drawBody(M5Canvas &c, bool alt) {
  drawRows(c, alt ? SPR_IDLE_B : SPR_IDLE_A, BODY_X, BODY_Y, SC, BODY_ROW0, BODY_ROW1, BODY_COL0);
}
static void sparkles(M5Canvas &c, int n) {
  for (int i = 0; i < n; i++)
    c.fillRect(esp_random() % (W - 4), esp_random() % (H - 4), 3, 3,
               (esp_random() & 1) ? TFT_WHITE : 0xFF08);
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
static const DSprite *lastSingSpr = nullptr;
static uint32_t lastFrame = 0, lastListenDraw = 0;
static uint16_t eqH[3] = {0, 0, 0};

static void drawStrip(M5Canvas &c) {
  c.fillCircle(8, H - 8, 4, dragonNetUp ? 0x34DF : 0x7BEF);
  if (lowBattery()) {
    c.drawRect(W - 22, H - 12, 14, 7, TFT_WHITE);
    c.fillRect(W - 8, H - 10, 2, 3, TFT_WHITE);
  }
}

static void drawSingBackdrop() {
  scene.fillSprite(TFT_BLACK);
  switch (dragonMood) {
    case MOOD_UPBEAT:
      scene.fillTriangle(0, 0, 40, 0, 0, 120, 0x39C7);
      scene.fillTriangle(W, 0, W - 40, 0, W, 120, 0x39C7);
      for (int i = 0; i < 12; i++) scene.fillRect(esp_random() % W, STRIP_Y + esp_random() % 28, 2, 2, TFT_WHITE);
      break;
    case MOOD_SAD:
      scene.fillSprite(0x000B);
      for (int i = 0; i < 8; i++) scene.fillRect(esp_random() % W, esp_random() % H, 1, 6, 0x2124);
      break;
    case MOOD_SLEEPY:
      drawSpr(scene, SPR_PROP_ZZ, W - 22, 6, 3);
      break;
    default:
      sparkles(scene, 4);
  }
  drawBody(scene, false);
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
      lifeEnter(L_FLOURISH, 2200);
    } else if (wasErr) {
      wasErr = false;
      lifeEnter(L_DIZZY, 2000);
    } else {
      lifeEnter(L_IDLE, 0);
    }
  } else if (s == FACE_SING) {
    M5.Speaker.stop();                           // audio guard: clean slate
    jaw = 0; wideRun = 0; lastSingSpr = nullptr;
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
  const DSprite *s;
  if (jaw < 64) { s = &SPR_SING_CLOSED; wideRun = 0; }
  else if (jaw < 160) { s = &SPR_SING_HALF; wideRun = 0; }
  else {
    wideRun = (uint8_t)min((int)wideRun + 1, 30);
    s = (wideRun >= 8) ? &SPR_SING_WIDE_SQUINT : &SPR_SING_WIDE;
  }
  if (s == lastSingSpr) return;
  lastSingSpr = s;
  singHead.fillSprite(dragonMood == MOOD_SAD ? 0x000B : TFT_BLACK);
  drawRows(singHead, *s, 0, 0, SC, HEAD_ROW0, HEAD_ROW1, HEAD_COL0);
  singHead.pushSprite(HEAD_X, HEAD_Y);
}

static void faceListenTick(const int16_t *buf, size_t n) {
  uint32_t now = millis();
  if (now - lastListenDraw < 66 || !dSpritesOk) return;
  lastListenDraw = now;
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i += 4) acc += abs(buf[i]);
  uint16_t h = min((uint32_t)26, (acc / (n / 4)) / 140);
  eqH[2] = eqH[1]; eqH[1] = eqH[0]; eqH[0] = h;
  scene.fillSprite(TFT_BLACK);
  drawHead(scene, SPR_SING_CLOSED);
  drawEyes(scene, EYE_OPEN, 0, 0);
  drawBody(scene, false);
  for (int i = 0; i < 3; i++)
    scene.fillRect(52 + i * 12, H - 6 - eqH[i], 8, max((int)eqH[i], 3), 0x07FF);
  scene.fillCircle(W - 12, 12, ((now / 400) & 1) ? 7 : 5, 0xF800);
  drawStrip(scene);
  scene.pushSprite(0, 0);
}

// -------------------------------------------------------------- render ----
static void renderScene() {
  uint32_t now = millis();
  uint32_t el = now - lifeT0;
  scene.fillSprite(TFT_BLACK);
  const DSprite *head = &SPR_SING_CLOSED;
  uint8_t eyes = EYE_OPEN;
  int pdx = 0, pdy = 0;
  bool bodyAlt = (now / 500) & 1;

  if (faceState == FACE_ERROR || life == L_DIZZY) {
    head = &SPR_SING_HALF; eyes = EYE_SQUINT;
    for (int i = 0; i < 3; i++) {
      int a = (now / 100 + i * 120) % 360;
      int sx = HEAD_X + HEAD_W / 2 + ((a < 180 ? a : 360 - a) - 90) * 55 / 90;
      scene.fillRect(sx, 4 + i * 6, 4, 4, 0xFF08);
    }
    if (faceState == FACE_ERROR) {
      scene.setTextDatum(bottom_center);
      scene.setTextColor(0xFC60, TFT_BLACK);
      scene.drawString(errReason, W / 2, H - 2);
    }
  } else if (faceState == FACE_THINK) {
    uint32_t tel = now - faceStateT0;
    pdx = 1; pdy = -1;                              // eyes up and away
    head = ((tel / 700) & 1) ? &SPR_SING_HALF : &SPR_SING_CLOSED;
    if (tel >= 1100) {
      static const int freqs[3] = {523, 659, 784};
      int i = (int)((tel - 1100) / 200);
      if (i < 3 && ((tel - 1100) % 200) < 60) M5.Speaker.tone(freqs[i], 70);
      drawSpr(scene, SPR_PROP_NOTE, W - 26, max(4, 40 - (int)(tel - 1100) / 30), 3);
    }
    scene.setTextDatum(bottom_center);
    scene.setTextColor(TFT_WHITE, TFT_BLACK);
    scene.drawString(((now / 800) & 1) ? "?" : "...", W / 2, H - 2);
  } else if (life == L_ASLEEP) {
    eyes = EYE_CLOSED; bodyAlt = false;
    if ((now / 900) & 1) drawSpr(scene, SPR_PROP_ZZ, W - 24, 6, 3);
  } else if (life == L_SLEEPY) {
    eyes = EYE_HALF; bodyAlt = (now / 1000) & 1;
    if ((now / 3000) % 3 == 0) drawSpr(scene, SPR_PROP_ZZ, W - 24, 6, 2);
  } else if (life == L_WAKE) {
    // stir: closed -> half -> a yawn -> open
    if (el < 400) eyes = EYE_CLOSED;
    else if (el < 800) eyes = EYE_HALF;
    else if (el < 1400) { eyes = EYE_CLOSED; head = &SPR_SING_WIDE; }
    else eyes = EYE_OPEN;
  } else if (life == L_STARTLE) {
    head = &SPR_SING_WIDE; eyes = EYE_OPEN; bodyAlt = (now / 120) & 1;
    scene.setTextDatum(top_right);
    scene.setTextColor(TFT_WHITE, TFT_BLACK);
    scene.drawString("!", W - 4, 2);
  } else if (life == L_FLOURISH) {
    switch (flourishMood) {
      case MOOD_SAD:    eyes = EYE_SAD; head = &SPR_SING_CLOSED; break;
      case MOOD_SLEEPY: eyes = EYE_HALF; break;
      default:
        eyes = EYE_HAPPY; head = (el < 1200) ? &SPR_SING_WIDE_SQUINT : &SPR_SING_CLOSED;
        sparkles(scene, 3);
        drawSpr(scene, SPR_PROP_NOTE, 8 + (el / 9) % 100, max(4, 60 - (int)el / 25), 3);
        if (el < 60) M5.Speaker.tone(660, 50);
    }
  } else {
    // plain idle: blink, occasional glance
    if (now >= nextBlink) { blinkUntil = now + 120; nextBlink = now + 2500 + esp_random() % 3000; }
    if (now < blinkUntil) eyes = EYE_BLINK;
    if (life == L_GLANCE) pdx = glanceDx;
    if (dragonMood == MOOD_SAD) eyes = (now < blinkUntil) ? EYE_BLINK : EYE_SAD;
  }

  drawHead(scene, *head);
  if (faceState != FACE_SING) drawEyes(scene, eyes, pdx, pdy);
  drawBody(scene, bodyAlt);
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

  // ---- FACE_IDLE: the face lives ----
  if (wantStartle) {
    wantStartle = false;
    lastActivity = now;
    if (life == L_ASLEEP || life == L_SLEEPY) {
      M5.Display.setBrightness(200);
      lifeEnter(L_WAKE, 1800);                   // picked up: stir, yawn, open
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
        glanceDx = (esp_random() & 1) ? 1 : -1;
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
  singHead.setColorDepth(16);
  dSpritesOk = scene.createSprite(W, H) && singHead.createSprite(HEAD_W, HEAD_H);
  if (!dSpritesOk) Serial.println("dbg FATAL: bigface sprite alloc failed");
  lastActivity = lastProtoEvent = millis();
  nextBlink = millis() + 1500;
  nextGlance = millis() + 4000;
  lifeEnter(L_WAKE, 1800);                       // boot = wake up under the hat
}
