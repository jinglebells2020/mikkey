// Mikkey the tamagotchi dragon — presentation layer.
// Implements the five hooks the protocol code calls:
//   faceSetState(FaceState) / faceTick() / singMouthFrame(env) /
//   faceListenTick(buf,n) / faceError(reason)
// Life: gravity-aware pet (IMU) — stands on the real "down" edge, tumbles on
// tilt, startles on shake, flies against true gravity, naps in a cave.
// Physics: integer 8.8 fixed point, 20fps tick. During SING everything is
// frozen and exactly one 120x96 head-region push happens per envelope frame.
#pragma once
#include <M5Unified.h>
#include "face_types.h"
#include "dragon_art.h"

// ---------------------------------------------------------------- render ----
static M5Canvas scene(&M5.Display);      // full screen 240x135 (PSRAM ok)
static M5Canvas singHead(&M5.Display);   // 120x96 close-up (DRAM, speed-critical)
static bool dSpritesOk = false;

static FaceState faceState = FACE_BOOT;
static uint32_t faceStateT0 = 0;
static bool dragonNetUp = false;   // set by tryNet(); drawn as the net dot
static const DSprite *lastSingSpr = nullptr;
static const char *FACE_NAMES2[] = {"BOOT", "IDLE", "LISTEN", "THINK", "SING", "ERROR"};

// edge: which screen edge is the floor
enum { EDGE_BOTTOM = 0, EDGE_TOP, EDGE_LEFT, EDGE_RIGHT };

// draw a fat-pixel sprite into a canvas. rot = floor edge (feet point there).
static void drawSpr(M5Canvas &c, const DSprite &s, int x, int y, int sc,
                    bool flip, uint8_t rot) {
  for (int cy = 0; cy < s.h; cy++) {
    for (int cx = 0; cx < s.w; cx++) {
      uint8_t v = s.px[cy * s.w + (flip ? (s.w - 1 - cx) : cx)];
      if (!v) continue;
      int ox, oy;
      switch (rot) {
        case EDGE_TOP:   ox = s.w - 1 - cx; oy = s.h - 1 - cy; break;
        case EDGE_LEFT:  ox = s.h - 1 - cy; oy = cx; break;
        case EDGE_RIGHT: ox = cy;           oy = s.w - 1 - cx; break;
        default:         ox = cx;           oy = cy; break;
      }
      c.fillRect(x + ox * sc, y + oy * sc, sc, sc, DPAL[v]);
    }
  }
}
static int sprW(const DSprite &s, int sc, uint8_t rot) {
  return (rot == EDGE_LEFT || rot == EDGE_RIGHT) ? s.h * sc : s.w * sc;
}
static int sprH(const DSprite &s, int sc, uint8_t rot) {
  return (rot == EDGE_LEFT || rot == EDGE_RIGHT) ? s.w * sc : s.h * sc;
}

// --------------------------------------------------------------- physics ----
// 8.8 fixed point. See design spec: gravity filter, edge hysteresis, tumble,
// shake, fly steering, gyro dizzy.
static const int32_t GRAV_NUM = 90, FLY_GRAV_NUM = 40;
static const int32_t V_FALL_MAX = 3072, V_FLY_MAX = 1024, WALK_V = 384;
static const int32_t REST_NUM = 77, V_BOUNCE_MIN = 384, FRIC_NUM = 216, V_STATIC = 32;
static const int32_t G_DEADBAND = 200, G_SWITCH_MIN = 300;
static const uint8_t EDGE_DEBOUNCE = 4;
static const int32_t SHAKE_MG = 500;
static const uint8_t STARTLE_CD_T = 30;
static const int32_t STEER_NUM = 12, STEER_MAX = 120, DAMP_NUM = 20, FLAP_IMP = 300;

enum { M_GROUND, M_TUMBLE, M_FLY, M_FROZEN };

static struct {
  int32_t px, py, vx, vy;         // 8.8
  uint8_t edge, cand, candTicks;
  bool grounded;
  uint8_t mode;
  uint8_t startleCd, flapPhase, shakeHits, gyroRun;
  uint16_t dizzyTicks;
  int32_t gfx, gfy, gfz;          // low-passed gravity, mg
  int32_t sx, sy;                 // runtime sign fix
  bool wantStartle, wantTumble;
} B = {60 << 8, 40 << 8, 0, 0, EDGE_BOTTOM, EDGE_BOTTOM, 0, false,
       M_GROUND, 0, 0, 0, 0, 0, 0, 1000, 0, 1, 1, false, false};

static int bodyW = 80, bodyH = 65;   // current AABB, px (16x13 @ 5, swaps on edge)
static int32_t flyTx = 120, flyTy = 60;

static void bodyAabbForEdge() {
  if (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) { bodyW = 65; bodyH = 80; }
  else { bodyW = 80; bodyH = 65; }
}

static void physCalibrate() {
  float fx, fy, fz;
  int32_t sxx = 0, syy = 0, szz = 0;
  int got = 0;
  for (int i = 0; i < 16; i++) {
    if (M5.Imu.getAccel(&fx, &fy, &fz)) {
      // measured on this unit: ax = screen-right, ay = screen-down (rot 1)
      sxx += (int32_t)(fx * 1000);
      syy += (int32_t)(fy * 1000);
      szz += (int32_t)(fz * 1000);
      got++;
    }
    delay(20);
  }
  if (!got) return;
  int32_t bx = sxx / got, by = syy / got, bz = szz / got;
  int32_t mag = abs(bx) + abs(by) + abs(bz);
  if (mag > 700 && mag < 1600) {
    // signs are hard-measured on this unit — no runtime auto-fix
    B.gfx = bx; B.gfy = by; B.gfz = bz;
    if (abs(B.gfx) < 250 && abs(B.gfy) < 250) { B.gfy = 1000; }
  } else {
    B.gfy = 1000;
  }
  B.edge = (abs(B.gfy) >= abs(B.gfx)) ? (B.gfy > 0 ? EDGE_BOTTOM : EDGE_TOP)
                                      : (B.gfx > 0 ? EDGE_RIGHT : EDGE_LEFT);
  bodyAabbForEdge();
  Serial.printf("dbg imu cal g=(%ld,%ld) edge=%d\n", (long)B.gfx, (long)B.gfy, B.edge);
}

static int32_t dbgAx = 0, dbgAy = 0, dbgAz = 0;

static void physSample() {
  float fx, fy, fz;
  if (!M5.Imu.getAccel(&fx, &fy, &fz)) return;
  dbgAx = (int32_t)(fx * 1000); dbgAy = (int32_t)(fy * 1000); dbgAz = (int32_t)(fz * 1000);
  static uint32_t dbgT = 0;
  if (millis() - dbgT > 2000) {
    dbgT = millis();
    Serial.printf("dbg acc raw ax=%ld ay=%ld az=%ld -> gf=(%ld,%ld) edge=%d\n",
                  (long)dbgAx, (long)dbgAy, (long)dbgAz,
                  (long)B.gfx, (long)B.gfy, B.edge);
  }
  // measured on this unit: ax = screen-right, ay = screen-down (rot 1)
  int32_t rx = (int32_t)(fx * 1000);
  int32_t ry = (int32_t)(fy * 1000);
  int32_t rz = (int32_t)(fz * 1000);
  B.gfx += (rx - B.gfx) >> 3;
  B.gfy += (ry - B.gfy) >> 3;
  B.gfz += (rz - B.gfz) >> 3;
  // shake: L1 high-pass
  int32_t hp = abs(rx - B.gfx) + abs(ry - B.gfy) + abs(rz - B.gfz);
  if (hp > SHAKE_MG) B.shakeHits = min(B.shakeHits + 2, 12);
  else if (B.shakeHits) B.shakeHits--;
  if (B.shakeHits >= 6 && !B.startleCd && B.mode != M_FROZEN && B.mode != M_TUMBLE) {
    B.wantStartle = true;
    B.shakeHits = 0;
    B.startleCd = STARTLE_CD_T;
  }
  if (B.startleCd) B.startleCd--;
  // gyro dizzy
  float gx, gy, gz;
  if (M5.Imu.getGyro(&gx, &gy, &gz)) {
    int32_t gsum = abs((int)gx) + abs((int)gy) + abs((int)gz);
    if (gsum > 500) { if (++B.gyroRun >= 2) B.dizzyTicks = 20; }
    else B.gyroRun = 0;
  }
  if (B.dizzyTicks) B.dizzyTicks--;
}

static void physTick() {
  physSample();
  if (B.mode == M_FROZEN) return;

  if (B.mode == M_FLY) {
    int32_t dx = (flyTx << 8) - B.px, dy = (flyTy << 8) - B.py;
    int32_t ax = constrain((dx * STEER_NUM) >> 8, -STEER_MAX, STEER_MAX) - ((B.vx * DAMP_NUM) >> 8);
    int32_t ay = constrain((dy * STEER_NUM) >> 8, -STEER_MAX, STEER_MAX) - ((B.vy * DAMP_NUM) >> 8);
    ax += (B.gfx * FLY_GRAV_NUM) >> 8;
    ay += (B.gfy * FLY_GRAV_NUM) >> 8;
    if (B.flapPhase == 0) {
      int32_t m = max(abs(B.gfx), abs(B.gfy)) + (min(abs(B.gfx), abs(B.gfy)) >> 1);
      if (m > 200) {
        B.vx -= ((B.gfx * 256 / m) * FLAP_IMP) >> 8;
        B.vy -= ((B.gfy * 256 / m) * FLAP_IMP) >> 8;
      } else {
        B.vy -= FLAP_IMP;
      }
    }
    B.flapPhase = (B.flapPhase + 1) % 6;
    B.vx = constrain(B.vx + ax, -V_FLY_MAX, V_FLY_MAX);
    B.vy = constrain(B.vy + ay, -V_FLY_MAX, V_FLY_MAX);
  } else {
    B.vx = constrain(B.vx + ((B.gfx * GRAV_NUM) >> 8), -V_FALL_MAX, V_FALL_MAX);
    B.vy = constrain(B.vy + ((B.gfy * GRAV_NUM) >> 8), -V_FALL_MAX, V_FALL_MAX);
  }
  B.px += B.vx;
  B.py += B.vy;

  // walls + restitution + friction
  bool onFloor = false;
  int32_t maxX = (240 - bodyW) << 8, maxY = (135 - bodyH) << 8;
  struct { bool hit; uint8_t e; } contact = {false, 0};
  if (B.px < 0)    { B.px = 0;    if (B.vx < 0) { B.vx = -(B.vx * REST_NUM) >> 8; if (abs(B.vx) < V_BOUNCE_MIN) B.vx = 0; } contact = {true, EDGE_LEFT}; }
  if (B.px > maxX) { B.px = maxX; if (B.vx > 0) { B.vx = -(B.vx * REST_NUM) >> 8; if (abs(B.vx) < V_BOUNCE_MIN) B.vx = 0; } contact = {true, EDGE_RIGHT}; }
  if (B.py < 0)    { B.py = 0;    if (B.vy < 0) { B.vy = -(B.vy * REST_NUM) >> 8; if (abs(B.vy) < V_BOUNCE_MIN) B.vy = 0; } contact = {true, EDGE_TOP}; }
  if (B.py > maxY) { B.py = maxY; if (B.vy > 0) { B.vy = -(B.vy * REST_NUM) >> 8; if (abs(B.vy) < V_BOUNCE_MIN) B.vy = 0; } contact = {true, EDGE_BOTTOM}; }
  if (contact.hit && contact.e == B.edge) {
    int32_t vn = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? B.vx : B.vy;
    if (vn == 0) {
      onFloor = true;
      int32_t *vt = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? &B.vy : &B.vx;
      *vt = (*vt * FRIC_NUM) >> 8;
      if (abs(*vt) < V_STATIC) *vt = 0;
      if (B.mode == M_TUMBLE) B.mode = M_GROUND;   // feet-first landing
    }
  }
  B.grounded = onFloor;

  // ground-edge selection with hysteresis -> tumble
  int32_t axm = abs(B.gfx), aym = abs(B.gfy);
  uint8_t cand = B.edge;
  if (max(axm, aym) >= G_SWITCH_MIN && max(axm, aym) >= G_DEADBAND) {
    bool curVert = (B.edge == EDGE_BOTTOM || B.edge == EDGE_TOP);
    int32_t curAxis = curVert ? aym : axm, othAxis = curVert ? axm : aym;
    if (othAxis > curAxis + (curAxis >> 2))
      cand = curVert ? (B.gfx > 0 ? EDGE_RIGHT : EDGE_LEFT)
                     : (B.gfy > 0 ? EDGE_BOTTOM : EDGE_TOP);
    else
      cand = curVert ? (B.gfy > 0 ? EDGE_BOTTOM : EDGE_TOP)
                     : (B.gfx > 0 ? EDGE_RIGHT : EDGE_LEFT);
  }
  if (cand != B.edge) {
    if (++B.candTicks >= EDGE_DEBOUNCE) {
      bool wasGrounded = B.grounded;
      B.edge = cand;
      B.candTicks = 0;
      bodyAabbForEdge();
      if (wasGrounded && B.mode == M_GROUND) {
        B.wantTumble = true;
        B.mode = M_TUMBLE;
        B.grounded = false;
        // pop off the old floor with some spice
        B.vx += (int32_t)(esp_random() & 255) - 128;
        B.vy += (int32_t)(esp_random() & 255) - 128;
      }
      Serial.printf("dbg floor -> %d\n", B.edge);
    }
  } else {
    B.candTicks = 0;
  }
}

// ------------------------------------------------------------------ life ----
enum Life { L_HATCH, L_PERCH, L_WANDER, L_FLY, L_GROOM, L_WATCH,
            L_CAVE_NAP, L_PEEK, L_STARTLE, L_TUMBLE, L_FLOURISH, L_DIZZY,
            L_YAWNWALK, L_DEEPSLEEP };
static uint8_t life = L_HATCH;
static bool moonwalk = false;      // fedora physics: sometimes he glides backward
static uint32_t lifeT0 = 0, lifeDwell = 2600;
static uint32_t lastActivity2 = 0;
static bool sleepy = false;            // battery < 20%
static int walkDir = 1;
static char errReason[20] = "";
static uint32_t nextBlink2 = 0;
static bool blinkOn = false;
static uint8_t savedLife = L_PERCH;

static void lifeEnter(uint8_t s, uint32_t mn, uint32_t mx) {
  life = s;
  lifeT0 = millis();
  lifeDwell = mn + (mx > mn ? esp_random() % (mx - mn) : 0);
}

static void lifePick() {
  uint32_t r = esp_random() % 100;
  switch (life) {
    case L_PERCH:
      if (r < 35 && !sleepy) lifeEnter(L_WANDER, 4000, 9000);
      else if (r < 60 && !sleepy) lifeEnter(L_FLY, 6000, 12000);
      else if (r < 75) lifeEnter(L_GROOM, 2500, 4000);
      else if (r < 90) lifeEnter(L_WATCH, 2000, 5000);
      else lifeEnter(L_PERCH, 3000, 7000);
      break;
    case L_WANDER:
      if (r < 40) lifeEnter(L_PERCH, 3000, 7000);
      else if (r < 60 && !sleepy) lifeEnter(L_FLY, 6000, 12000);
      else if (r < 75) lifeEnter(L_CAVE_NAP, sleepy ? 16000 : 8000, sleepy ? 40000 : 20000);
      else lifeEnter(L_WATCH, 2000, 5000);
      break;
    case L_FLY:
      if (r < 60) lifeEnter(L_PERCH, 3000, 7000);
      else if (r < 85) lifeEnter(L_WANDER, 4000, 9000);
      else lifeEnter(L_WATCH, 2000, 5000);
      break;
    case L_CAVE_NAP: lifeEnter(L_PEEK, 1500, 3000); break;
    case L_PEEK:
      if (r < 70) lifeEnter(L_WANDER, 4000, 9000);
      else lifeEnter(L_CAVE_NAP, 8000, 20000);
      break;
    default: lifeEnter(L_PERCH, 3000, 7000);
  }
  if (B.mode == M_FLY) B.mode = M_GROUND;   // pop stars dance, they don't fly
  if (life == L_WANDER) moonwalk = (esp_random() & 1);   // half his walks are moonwalks
}

// ---------------------------------------------------------------- drawing ----
static const uint16_t C_CAVE = 0x632C, C_CAVE_D = 0x2104;

static void drawCave(M5Canvas &c) {
  // rock arch anchored at the "left" corner of the current floor edge
  switch (B.edge) {
    case EDGE_BOTTOM:
      c.fillRoundRect(0, 135 - 54, 62, 54, 14, C_CAVE);
      c.fillRoundRect(10, 135 - 40, 42, 40, 10, C_CAVE_D);
      break;
    case EDGE_TOP:
      c.fillRoundRect(178, 0, 62, 54, 14, C_CAVE);
      c.fillRoundRect(188, 0, 42, 40, 10, C_CAVE_D);
      break;
    case EDGE_LEFT:
      c.fillRoundRect(0, 0, 54, 62, 14, C_CAVE);
      c.fillRoundRect(0, 10, 40, 42, 10, C_CAVE_D);
      break;
    case EDGE_RIGHT:
      c.fillRoundRect(240 - 54, 135 - 62, 54, 62, 14, C_CAVE);
      c.fillRoundRect(240 - 40, 135 - 52, 40, 42, 10, C_CAVE_D);
      break;
  }
}

// cave mouth center in screen coords (where the dragon curls up)
static void caveSpot(int *x, int *y) {
  switch (B.edge) {
    case EDGE_BOTTOM: *x = 4;   *y = 135 - 55; break;
    case EDGE_TOP:    *x = 182; *y = 0; break;
    case EDGE_LEFT:   *x = 0;   *y = 4; break;
    default:          *x = 240 - 60; *y = 135 - 66; break;
  }
}

static void drawBatteryIfLow(M5Canvas &c) {
  static int lvl = 100;
  static uint32_t nextPoll = 0;
  if (millis() > nextPoll) {
    nextPoll = millis() + 30000;
    lvl = M5.Power.getBatteryLevel();
    sleepy = (lvl >= 0 && lvl < 20);
    if (sleepy) Serial.printf("dbg batt LOW %d\n", lvl);
  }
  if (sleepy) {
    c.drawRect(222, 4, 14, 7, TFT_WHITE);
    c.fillRect(236, 6, 2, 3, TFT_WHITE);
    c.fillRect(224, 6, max(1, lvl * 10 / 100), 3, lvl < 10 ? 0xF800 : 0xFD20);
  }
}

static void sparkles(M5Canvas &c, int n) {
  for (int i = 0; i < n; i++)
    c.fillRect(esp_random() % 234, esp_random() % 129, 3, 3,
               (esp_random() & 1) ? TFT_WHITE : 0xFF08);
}

// pick current pose sprite for the mobile dragon
static const DSprite *poseSprite(bool *pFlip) {
  uint32_t now = millis();
  *pFlip = (walkDir < 0);
  if (B.mode == M_TUMBLE) return &SPR_STARTLED;
  if (B.dizzyTicks) return &SPR_STARTLED;
  switch (life) {
    case L_WANDER:
      // moonwalk: face one way, glide the other — the legs still cycle
      *pFlip = moonwalk ? (walkDir > 0) : (walkDir < 0);
      return ((now / 180) & 1) ? &SPR_WALK_A : &SPR_WALK_B;
    case L_YAWNWALK:
      return ((now / 180) & 1) ? &SPR_WALK_A : &SPR_WALK_B;
    case L_FLY: {   // dance break: spin - kick - spin
      static const DSprite *dn[4] = {&SPR_SPIN_A, &SPR_SPIN_B, &SPR_KICK, &SPR_SPIN_A};
      *pFlip = (walkDir < 0);
      return dn[(now / 220) & 3];
    }
    case L_STARTLE: return &SPR_STARTLED;
    default: return nullptr;   // big idle handled separately
  }
}

// ------------------------------------------------------- protocol hooks ----
static uint32_t lastFrame2 = 0;
static uint8_t jaw = 0;            // smoothed jaw for SING
static uint8_t wideRun = 0;
static uint32_t lastListenDraw = 0;
static uint16_t eqH[3] = {0, 0, 0};
static bool wasSing = false, wasErr = false;

static void renderScene();   // fwd

static void faceSetState(FaceState s) {
  if (faceState == FACE_SING && s == FACE_IDLE) wasSing = true;
  faceState = s;
  faceStateT0 = millis();
  lastActivity2 = millis();
  Serial.printf("dbg face -> %s\n", FACE_NAMES2[s]);
  M5.Display.setBrightness(200);
  if (s == FACE_IDLE) {
    if (wasSing) { wasSing = false; savedLife = L_WATCH; lifeEnter(L_FLOURISH, 1800, 1801); }
    else if (wasErr) { wasErr = false; lifeEnter(L_DIZZY, 2500, 2501); }
    else if (life == L_DEEPSLEEP || life == L_CAVE_NAP) lifeEnter(L_PERCH, 3000, 7000);
    B.mode = (life == L_FLY) ? M_FLY : M_GROUND;
  } else if (s == FACE_SING) {
    B.mode = M_FROZEN;
    jaw = 0; wideRun = 0; lastSingSpr = nullptr;
    if (dSpritesOk) {                 // stage backdrop drawn ONCE, pre-slurp
      scene.fillSprite(TFT_BLACK);
      sparkles(scene, 3);
      scene.pushSprite(0, 0);
    }
  } else if (s == FACE_LISTEN || s == FACE_THINK) {
    B.mode = M_FROZEN;
  } else if (s == FACE_ERROR) {
    B.mode = M_GROUND;
  }
}

static void faceError(const char *reason) {
  Serial.printf("dbg face ERROR: %s\n", reason);
  strncpy(errReason, reason, sizeof(errReason) - 1);
  wasErr = true;
  faceSetState(FACE_ERROR);
}

// jaw thresholds from the artist: <64 closed, 64-159 half, >=160 wide,
// wide 8+ consecutive frames -> squint (belting)
static void singMouthFrame(uint8_t env) {
  if (!dSpritesOk) return;
  uint8_t target = env;
  if (target > jaw) jaw += (target - jaw) >> 1;         // fast attack
  else jaw -= (jaw - target) >> 2;                      // slow release
  const DSprite *s;
  if (jaw < 64) { s = &SPR_SING_CLOSED; wideRun = 0; }
  else if (jaw < 160) { s = &SPR_SING_HALF; wideRun = 0; }
  else {
    wideRun = (uint8_t)min((int)wideRun + 1, 30);
    s = (wideRun >= 8) ? &SPR_SING_WIDE_SQUINT : &SPR_SING_WIDE;
  }
  if (s == lastSingSpr) return;                         // dedup: no change, no push
  lastSingSpr = s;
  singHead.fillSprite(TFT_BLACK);
  drawSpr(singHead, *s, 0, 0, 6, false, EDGE_BOTTOM);
  singHead.pushSprite(60, 20);
}

static void faceListenTick(const int16_t *buf, size_t n) {
  uint32_t now = millis();
  if (now - lastListenDraw < 66 || !dSpritesOk) return;
  lastListenDraw = now;
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i += 4) acc += abs(buf[i]);
  uint16_t h = min((uint32_t)40, (acc / (n / 4)) / 90);
  eqH[2] = eqH[1]; eqH[1] = eqH[0]; eqH[0] = h;
  scene.fillSprite(TFT_BLACK);
  drawSpr(scene, SPR_SING_CLOSED, 30, 20, 6, false, EDGE_BOTTOM);   // leans in close
  // eq bars beside the head
  for (int i = 0; i < 3; i++) {
    scene.fillRect(174 + i * 14, 110 - eqH[i], 10, max((int)eqH[i], 3), 0x07FF);
  }
  scene.fillCircle(224, 14, ((now / 400) & 1) ? 8 : 6, 0xF800);     // rec dot
  scene.pushSprite(0, 0);
}

static void renderScene() {
  scene.fillSprite(TFT_BLACK);
  uint32_t now = millis();
  int px = B.px >> 8, py = B.py >> 8;

  if (life == L_CAVE_NAP || life == L_DEEPSLEEP || life == L_PEEK) {
    drawCave(scene);
    int cx, cy;
    caveSpot(&cx, &cy);
    if (life == L_PEEK) {
      // head pokes out of the cave mouth
      drawSpr(scene, ((now / 400) & 1) ? SPR_WALK_A : SPR_WALK_B, cx + 20, cy + 10, 4,
              B.edge == EDGE_TOP || B.edge == EDGE_RIGHT, B.edge);
    } else {
      drawSpr(scene, ((now / 1000) & 1) ? SPR_SLEEP_A : SPR_SLEEP_B, cx, cy + 4, 4,
              false, B.edge);
      if ((now / 900) & 1) drawSpr(scene, SPR_PROP_ZZ, cx + 60, cy - 14, 3, false, EDGE_BOTTOM);
    }
  } else if (life == L_HATCH) {
    // spotlight entrance: beam grows, the star fades in, sparkles
    uint32_t el = now - lifeT0;
    int r = min((int)(el / 18), 78);
    scene.fillCircle(120, 96, r, 0x39C7);            // dim pool of light
    scene.fillCircle(120, 96, max(0, r - 10), 0x7BCF);
    if (el > 900) {
      drawSpr(scene, ((now / 500) & 1) ? SPR_IDLE_A : SPR_IDLE_B,
              65, 135 - 90, 5, false, EDGE_BOTTOM);
    }
    if (el > 1400) sparkles(scene, 4);
  } else if (life == L_FLOURISH) {
    // showtime: spin -> fire breath -> hold the pose, sparkles throughout
    uint32_t el = now - lifeT0;
    if (el < 800) {
      drawSpr(scene, ((now / 110) & 1) ? SPR_SPIN_A : SPR_SPIN_B, 70, 15, 5, false, EDGE_BOTTOM);
    } else if (el < 1500) {
      drawSpr(scene, SPR_LEAN, 70, 40, 6, false, EDGE_BOTTOM);   // the impossible lean
    } else {
      drawSpr(scene, SPR_IDLE_A, 70, 20, 5, false, EDGE_BOTTOM);   // the pose
      drawSpr(scene, SPR_PROP_NOTE, 40 + (el / 9) % 160, 20 + ((el / 13) % 40), 3, false, EDGE_BOTTOM);
    }
    sparkles(scene, 5);
  } else if (life == L_DIZZY || faceState == FACE_ERROR) {
    uint8_t rot = (now / 200) % 4;
    drawSpr(scene, SPR_STARTLED, 70, 20, 5, false, rot);
    for (int i = 0; i < 3; i++) {
      int a = (now / 100 + i * 120) % 360;
      int sx2 = 120 + ((a < 180 ? a : 360 - a) - 90);
      scene.fillRect(sx2, 14 + i * 6, 4, 4, 0xFF08);
    }
    scene.setTextDatum(bottom_center);
    scene.setTextColor(0xFC60, TFT_BLACK);
    scene.drawString(errReason, 120, 133);
  } else if (faceState == FACE_THINK) {
    int bob = ((now / 300) & 1) ? 1 : 0;
    drawSpr(scene, ((now / 500) & 1) ? SPR_IDLE_A : SPR_IDLE_B, 65, 22 + bob, 5, false, EDGE_BOTTOM);
    scene.setTextDatum(top_center);
    scene.setTextColor(TFT_WHITE, TFT_BLACK);
    scene.drawString(((now / 800) & 1) ? "?" : "...", 130, 8);
  } else if (life == L_WATCH) {
    // comes right up to the camera: big head, straight-on
    drawSpr(scene, SPR_SING_CLOSED, 45, 12, 7, false, EDGE_BOTTOM);
  } else {
    drawCave(scene);
    bool flip;
    const DSprite *s = poseSprite(&flip);
    if (s) {
      drawSpr(scene, *s, px, py, 5, flip, B.edge);
    } else {
      // grounded idle: the big bouncing front view, feet on the floor edge
      const DSprite &idle = (life == L_STARTLE) ? SPR_STARTLED
                            : (((now / 500) & 1) ? SPR_IDLE_A : SPR_IDLE_B);
      int sc = 5;
      int w = sprW(idle, sc, B.edge), h = sprH(idle, sc, B.edge);
      int ix, iy;
      switch (B.edge) {
        case EDGE_BOTTOM: ix = (240 - w) / 2; iy = 135 - h; break;
        case EDGE_TOP:    ix = (240 - w) / 2; iy = 0; break;
        case EDGE_LEFT:   ix = 0; iy = (135 - h) / 2; break;
        default:          ix = 240 - w; iy = (135 - h) / 2; break;
      }
      drawSpr(scene, idle, ix, iy, sc, false, B.edge);
      if (life == L_STARTLE) {
        scene.setTextDatum(top_center);
        scene.setTextColor(TFT_WHITE, TFT_BLACK);
        scene.drawString("!", ix + w / 2, max(0, iy - 14));
      }
      if (sleepy && ((now / 3000) % 3 == 0)) {
        drawSpr(scene, SPR_PROP_ZZ, ix + w - 8, max(0, iy - 16), 2, false, EDGE_BOTTOM);
      }
    }
  }
  drawBatteryIfLow(scene);
  scene.fillCircle(8, 10, 4, dragonNetUp ? 0x34DF : 0x7BEF);   // link dot
  scene.pushSprite(0, 0);
}

static void faceTick() {
  uint32_t now = millis();
  if (faceState == FACE_SING || faceState == FACE_LISTEN) return;
  if (now - lastFrame2 < ((life == L_DEEPSLEEP) ? 200 : 50)) return;
  lastFrame2 = now;

  if (faceState == FACE_BOOT) {
    if (now - faceStateT0 > 400) faceSetState(FACE_IDLE);
    return;
  }
  if (faceState == FACE_THINK) {
    physSample();   // keep gravity warm
    renderScene();
    if (now - faceStateT0 > 15000) { Serial.println("dbg think timeout"); faceSetState(FACE_IDLE); }
    return;
  }
  if (faceState == FACE_ERROR) {
    physSample();
    renderScene();
    if (now - faceStateT0 > 2500) { wasErr = false; faceSetState(FACE_IDLE); lifeEnter(L_PERCH, 3000, 7000); }
    return;
  }

  // ---- FACE_IDLE: the pet lives ----
  physTick();

  if (B.wantStartle) {
    B.wantStartle = false;
    lastActivity2 = now;
    if (life == L_DEEPSLEEP || life == L_CAVE_NAP) M5.Display.setBrightness(200);
    M5.Speaker.tone(880, 40);
    lifeEnter(L_STARTLE, 1000, 1001);
  }
  if (B.wantTumble) {
    B.wantTumble = false;
    lastActivity2 = now;
    if (life == L_CAVE_NAP || life == L_DEEPSLEEP || life == L_PEEK)
      lifeEnter(L_TUMBLE, 3000, 3001);   // dumped out of bed
    else if (life != L_STARTLE)
      lifeEnter(L_TUMBLE, 3000, 3001);
  }

  // deep sleep after 45s of no interaction
  if (now - lastActivity2 > 45000 && life != L_DEEPSLEEP && life != L_YAWNWALK
      && life != L_CAVE_NAP) {
    lifeEnter(L_YAWNWALK, 6000, 6001);
  }

  switch (life) {
    case L_HATCH:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_PERCH, 3000, 7000);
      break;
    case L_WANDER: case L_YAWNWALK: {
      // walk along the floor; YAWNWALK heads for the cave
      int32_t *vt = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? &B.vy : &B.vx;
      int v = (life == L_YAWNWALK || sleepy) ? WALK_V / 2 : WALK_V;
      if (life == L_YAWNWALK) {
        int cx, cy;
        caveSpot(&cx, &cy);
        int32_t target = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? (cy << 8) : (cx << 8);
        int32_t pos = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? B.py : B.px;
        walkDir = (target > pos) ? 1 : -1;
        if (abs(target - pos) < (6 << 8)) { lifeEnter(L_DEEPSLEEP, 3600000, 3600001); M5.Display.setBrightness(90); break; }
      } else if ((esp_random() & 63) == 0) {
        walkDir = -walkDir;
      }
      if (B.grounded) *vt = walkDir * v;
      // reached a screen end: turn around
      if ((B.px >> 8) <= 1 || (B.px >> 8) >= 238 - bodyW) walkDir = -walkDir;
      if (now - lifeT0 > lifeDwell && life == L_WANDER) lifePick();
      break;
    }
    case L_FLY:
      if (((now - lifeT0) / 2500) & 1) { /* second lobe of the lazy 8 */
        flyTx = 60 + ((lifeT0 / 7) % 60);
      }
      if (now - lifeT0 > lifeDwell) lifePick();
      break;
    case L_STARTLE:
      if (now - lifeT0 > lifeDwell)
        lifeEnter((B.mode == M_TUMBLE) ? L_TUMBLE : L_PERCH, 3000, 7000);
      break;
    case L_TUMBLE:
      if (B.mode == M_GROUND && B.grounded) lifeEnter(L_PERCH, 3000, 7000);
      else if (now - lifeT0 > 4000) { B.mode = M_GROUND; lifeEnter(L_PERCH, 3000, 7000); }
      break;
    case L_FLOURISH:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_WATCH, 2000, 4000);
      break;
    case L_DIZZY:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_PERCH, 3000, 7000);
      break;
    case L_DEEPSLEEP:
      break;   // wakes only via events (button/protocol/startle)
    default:
      if (now - lifeT0 > lifeDwell) lifePick();
  }
  renderScene();
}

static void dragonInit() {
  scene.setColorDepth(16);
  scene.setPsram(true);
  singHead.setColorDepth(16);
  dSpritesOk = scene.createSprite(240, 135) && singHead.createSprite(120, 96);
  if (!dSpritesOk) Serial.println("dbg FATAL: dragon sprite alloc failed");
  physCalibrate();
  lastActivity2 = millis();
  lifeEnter(L_HATCH, 2600, 2601);
}
