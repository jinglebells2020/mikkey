// Mikkey — full-screen face for the 3D-printed hat.
// Portrait screen (135x240) = a 9x16 grid of 15px cells, drawn procedurally:
// edge-to-edge skin, big eyes, nose, blush, a mouth that spans the screen
// when he sings, rounded chin. The physical hat sits on the top edge, so the
// forehead is flush with it in every state: idle, listen, think, sing, error.
// Same five hooks the protocol code calls:
//   faceSetState / faceTick / singMouthFrame / faceListenTick / faceError
// plus dragonInit / dragonSetMood / dragonNetUp for source compatibility.
#pragma once
#include <M5Unified.h>
#include "face_types.h"
#include "dragon_art.h"

// ------------------------------------------------------------- layout ----
static const int W = 135, H = 240, SC = 15;                      // 9x16 cells
static const int EYE_ROW = 3, EYE_L = 1, EYE_R = 5;               // 3x3 eyes
static const int NOSE_ROW = 7, MOUTH_ROW = 9;                     // mouth rows 9..13
static const int MOUTH_Y = MOUTH_ROW * SC, MOUTH_H = 5 * SC;      // 135..210
static const int EYES_Y = EYE_ROW * SC, EYES_H = 3 * SC;          // 45..90
static const uint16_t C_SKIN = 0xF694, C_DARK = 0x2104, C_WHITE = 0xFFFF;
static const uint16_t C_MOUTH = 0xA800, C_BLUSH = 0xFB56, C_LIP = 0xDA09;

static M5Canvas scene(&M5.Display);      // 135x240 (PSRAM)
static M5Canvas mouthCv(&M5.Display);    // 135x75 mouth strip (DRAM, speed-critical)
static M5Canvas eyesCv(&M5.Display);     // 135x45 eyes strip (DRAM)
static bool dSpritesOk = false;

static FaceState faceState = FACE_BOOT;
static uint32_t faceStateT0 = 0;
static bool dragonNetUp = false;
static const char *FACE_NAMES2[] = {"BOOT", "IDLE", "LISTEN", "THINK", "SING", "ERROR"};

enum { MOOD_PLAIN = 0, MOOD_UPBEAT, MOOD_SAD, MOOD_SLEEPY };
static uint8_t dragonMood = MOOD_PLAIN;
static void dragonSetMood(uint8_t m) { dragonMood = m; Serial.printf("dbg mood=%d\n", m); }

static inline void cell(M5Canvas &c, int col, int row, uint16_t color, int oy = 0) {
  c.fillRect(col * SC, row * SC - oy, SC, SC, color);
}
static void drawSpr(M5Canvas &c, const DSprite &s, int x, int y, int sc) {
  for (int cy = 0; cy < s.h; cy++)
    for (int cx = 0; cx < s.w; cx++) {
      uint8_t v = s.px[cy * s.w + cx];
      if (v) c.fillRect(x + cx * sc, y + cy * sc, sc, sc, DPAL[v]);
    }
}

// ---------------------------------------------------------------- face ----
// Skin edge to edge; black rounded corners top (under the hat) and chin.
static void drawSkin(M5Canvas &c, uint16_t bg) {
  c.fillSprite(C_SKIN);
  cell(c, 0, 0, bg); cell(c, 8, 0, bg);
  cell(c, 0, 14, bg); cell(c, 8, 14, bg);
  cell(c, 0, 15, bg); cell(c, 1, 15, bg); cell(c, 7, 15, bg); cell(c, 8, 15, bg);
  cell(c, 4, NOSE_ROW, C_DARK);                       // nose
  cell(c, 0, NOSE_ROW, C_BLUSH); cell(c, 8, NOSE_ROW, C_BLUSH);
}

enum { MOUTH_CLOSED, MOUTH_HALF, MOUTH_WIDE };
// Drawn into a canvas whose y=0 is screen row MOUTH_ROW (oy = MOUTH_Y).
static void drawMouth(M5Canvas &c, uint8_t style, int oy) {
  for (int r = MOUTH_ROW; r < MOUTH_ROW + 5; r++)
    for (int k = 0; k < 9; k++) cell(c, k, r, C_SKIN, oy);
  switch (style) {
    case MOUTH_CLOSED:
      for (int k = 2; k <= 6; k++) cell(c, k, MOUTH_ROW + 1, C_DARK, oy);
      break;
    case MOUTH_HALF:
      for (int k = 2; k <= 6; k++) { cell(c, k, MOUTH_ROW + 1, C_DARK, oy); cell(c, k, MOUTH_ROW + 3, C_DARK, oy); }
      cell(c, 1, MOUTH_ROW + 2, C_DARK, oy); cell(c, 7, MOUTH_ROW + 2, C_DARK, oy);
      for (int k = 2; k <= 6; k++) cell(c, k, MOUTH_ROW + 2, C_MOUTH, oy);
      break;
    default:   // WIDE: lips, teeth, throat — spans the screen
      for (int k = 1; k <= 7; k++) { cell(c, k, MOUTH_ROW, C_DARK, oy); cell(c, k, MOUTH_ROW + 4, C_DARK, oy); }
      for (int r = 1; r <= 3; r++) { cell(c, 0, MOUTH_ROW + r, C_DARK, oy); cell(c, 8, MOUTH_ROW + r, C_DARK, oy); }
      for (int k = 1; k <= 7; k++) cell(c, k, MOUTH_ROW + 1, C_WHITE, oy);
      for (int k = 1; k <= 7; k++) { cell(c, k, MOUTH_ROW + 2, C_MOUTH, oy); cell(c, k, MOUTH_ROW + 3, C_MOUTH, oy); }
      break;
  }
}

// ---------------------------------------------------------------- eyes ----
// The eyes are 3x3 blocks at sprite rows 4..6, cols 7..9 and 14..16.
// Procedural so the big face can blink, glance, doze and squint.
enum { EYE_OPEN, EYE_BLINK, EYE_HALF, EYE_CLOSED, EYE_HAPPY, EYE_SQUINT, EYE_SAD };

static void drawEyes(M5Canvas &c, uint8_t style, int pdx, int pdy, int oy = 0) {
  for (int e = 0; e < 2; e++) {
    int col = e ? EYE_R : EYE_L;
    int x0 = col * SC, y0 = EYE_ROW * SC - oy;
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
static int8_t lastMouth = -1, lastSquint = -1;
static uint32_t lastFrame = 0, lastListenDraw = 0;
static uint16_t eqH[3] = {0, 0, 0};

static void drawStrip(M5Canvas &c) {
  c.fillCircle(8, H - 8, 4, dragonNetUp ? 0x34DF : 0x7BEF);   // in the chin corner
  if (lowBattery()) {
    c.drawRect(W - 22, H - 12, 14, 7, TFT_WHITE);
    c.fillRect(W - 8, H - 10, 2, 3, TFT_WHITE);
  }
}

static void drawSingBackdrop() {
  uint16_t bg = (dragonMood == MOOD_SAD) ? 0x000B : TFT_BLACK;
  drawSkin(scene, bg);
  drawEyes(scene, EYE_OPEN, 0, 0);
  drawMouth(scene, MOUTH_CLOSED, 0);
  if (dragonMood == MOOD_SLEEPY) drawSpr(scene, SPR_PROP_ZZ, W - 22, 4, 3);
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
  int8_t sq = (m == MOUTH_WIDE && wideRun >= 8) ? 1 : 0;   // big note: happy squint
  if (m != lastMouth) {
    lastMouth = m;
    drawMouth(mouthCv, m, MOUTH_Y);
    mouthCv.pushSprite(0, MOUTH_Y);
  }
  if (sq != lastSquint) {
    lastSquint = sq;
    for (int k = 0; k < 9; k++) for (int r = 0; r < 3; r++) eyesCv.fillRect(k * SC, r * SC, SC, SC, C_SKIN);
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
  uint16_t h = min((uint32_t)26, (acc / (n / 4)) / 140);
  eqH[2] = eqH[1]; eqH[1] = eqH[0]; eqH[0] = h;
  drawSkin(scene, TFT_BLACK);
  drawEyes(scene, EYE_OPEN, 0, 0);
  drawMouth(scene, MOUTH_CLOSED, 0);
  for (int i = 0; i < 3; i++)                        // listening meter on the chin
    scene.fillRect(52 + i * 12, H - 4 - eqH[i], 8, max((int)eqH[i], 3), 0x07FF);
  scene.fillCircle(W - 12, 12, ((now / 400) & 1) ? 7 : 5, 0xF800);
  drawStrip(scene);
  scene.pushSprite(0, 0);
}

// -------------------------------------------------------------- render ----
static void renderScene() {
  uint32_t now = millis();
  uint32_t el = now - lifeT0;
  uint16_t bg = TFT_BLACK;
  uint8_t mouth = MOUTH_CLOSED, eyes = EYE_OPEN;
  int pdx = 0, pdy = 0;
  drawSkin(scene, bg);

  if (faceState == FACE_ERROR || life == L_DIZZY) {
    mouth = MOUTH_HALF; eyes = EYE_SQUINT;
    for (int i = 0; i < 3; i++) {
      int a = (now / 100 + i * 120) % 360;
      int sx = W / 2 + ((a < 180 ? a : 360 - a) - 90) * 60 / 90;
      scene.fillRect(sx, 4 + i * 6, 5, 5, 0xFF08);
    }
    if (faceState == FACE_ERROR) {
      scene.setTextDatum(bottom_center);
      scene.setTextColor(0xFC60, C_SKIN);
      scene.drawString(errReason, W / 2, H - 2);
    }
  } else if (faceState == FACE_THINK) {
    uint32_t tel = now - faceStateT0;
    pdx = 1; pdy = -1;                              // eyes up and away
    mouth = ((tel / 700) & 1) ? MOUTH_HALF : MOUTH_CLOSED;
    if (tel >= 1100) {
      static const int freqs[3] = {523, 659, 784};
      int i = (int)((tel - 1100) / 200);
      if (i < 3 && ((tel - 1100) % 200) < 60) M5.Speaker.tone(freqs[i], 70);
      drawSpr(scene, SPR_PROP_NOTE, W - 24, max(2, 30 - (int)(tel - 1100) / 40), 3);
    }
    scene.setTextDatum(bottom_center);
    scene.setTextColor(C_DARK, C_SKIN);
    scene.drawString(((now / 800) & 1) ? "?" : "...", W / 2, H - 2);
  } else if (life == L_ASLEEP) {
    eyes = EYE_CLOSED;
    if ((now / 900) & 1) drawSpr(scene, SPR_PROP_ZZ, W - 24, 4, 3);
  } else if (life == L_SLEEPY) {
    eyes = EYE_HALF;
    if ((now / 3000) % 3 == 0) drawSpr(scene, SPR_PROP_ZZ, W - 24, 4, 2);
  } else if (life == L_WAKE) {
    // stir: closed -> half -> a yawn -> open
    if (el < 400) eyes = EYE_CLOSED;
    else if (el < 800) eyes = EYE_HALF;
    else if (el < 1400) { eyes = EYE_CLOSED; mouth = MOUTH_WIDE; }
    else eyes = EYE_OPEN;
  } else if (life == L_STARTLE) {
    mouth = MOUTH_WIDE; eyes = EYE_OPEN;
    scene.setTextDatum(top_right);
    scene.setTextColor(C_DARK, C_SKIN);
    scene.drawString("!", W - 4, 2);
  } else if (life == L_FLOURISH) {
    switch (flourishMood) {
      case MOOD_SAD:    eyes = EYE_SAD; break;
      case MOOD_SLEEPY: eyes = EYE_HALF; break;
      default:
        eyes = EYE_HAPPY; mouth = (el < 1200) ? MOUTH_WIDE : MOUTH_HALF;
        sparkles(scene, 3);
        drawSpr(scene, SPR_PROP_NOTE, 8 + (el / 9) % 100, max(2, 30 - (int)el / 40), 3);
        if (el < 60) M5.Speaker.tone(660, 50);
    }
  } else {
    // plain idle: blink, occasional glance
    if (now >= nextBlink) { blinkUntil = now + 120; nextBlink = now + 2500 + esp_random() % 3000; }
    if (now < blinkUntil) eyes = EYE_BLINK;
    if (life == L_GLANCE) pdx = glanceDx;
    if (dragonMood == MOOD_SAD) eyes = (now < blinkUntil) ? EYE_BLINK : EYE_SAD;
  }

  drawEyes(scene, eyes, pdx, pdy);
  drawMouth(scene, mouth, 0);
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
  mouthCv.setColorDepth(16);
  eyesCv.setColorDepth(16);
  dSpritesOk = scene.createSprite(W, H) && mouthCv.createSprite(W, MOUTH_H) && eyesCv.createSprite(W, EYES_H);
  if (!dSpritesOk) Serial.println("dbg FATAL: bigface sprite alloc failed");
  lastActivity = lastProtoEvent = millis();
  nextBlink = millis() + 1500;
  nextGlance = millis() + 4000;
  lifeEnter(L_WAKE, 1800);                       // boot = wake up under the hat
}
