// Mikkey the pixel pop star — presentation layer.
// Implements the five hooks the protocol code calls:
//   faceSetState(FaceState) / faceTick() / singMouthFrame(env) /
//   faceListenTick(buf,n) / faceError(reason)
// Personality: a warm, hammy, slightly vain little showman. Three meters —
// E(nergy), H(am, appetite for attention), G(rump, accumulated indignity) —
// drive his day: fresh -> show-off -> tired -> nap -> recharged. Applause
// cures everything (G=0 after every song). The fedora is a separate object:
// hat-tips, wake-up dressing, tumble hat-retrieval, encore hat-toss.
// Physics: IMU gravity (measured mapping: -ax=right, +ay=down), integer 8.8.
// During SING nothing renders but the 120x96 head close-up (backdrop drawn
// once pre-clip); no tones during SING, ever.
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
static bool dragonNetUp = false;
static const DSprite *lastSingSpr = nullptr;
static const char *FACE_NAMES2[] = {"BOOT", "IDLE", "LISTEN", "THINK", "SING", "ERROR"};

enum { EDGE_BOTTOM = 0, EDGE_TOP, EDGE_LEFT, EDGE_RIGHT };
enum { MOOD_PLAIN = 0, MOOD_UPBEAT, MOOD_SAD, MOOD_SLEEPY };
static uint8_t dragonMood = MOOD_PLAIN;
static void dragonSetMood(uint8_t m) { dragonMood = m; Serial.printf("dbg mood=%d\n", m); }

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

// ---- the hat as a separate object ----
static void hatEctomy(M5Canvas &c, int x, int y, int sc, bool wide22, uint16_t bg) {
  if (wide22) c.fillRect(x + 4 * sc, y, 14 * sc, 2 * sc, bg);
  else        c.fillRect(x + 3 * sc, y, 10 * sc, 2 * sc, bg);
}
static void hatSeat(int bodyX, int bodyY, int sc, bool wide22, int *hx, int *hy) {
  *hx = bodyX + (wide22 ? 6 : 3) * sc;
  *hy = bodyY - 1 * sc;
}
static void drawHatAt(M5Canvas &c, int x, int y, int sc, uint8_t rot) {
  drawSpr(c, SPR_HAT, x, y, sc, false, rot);
}

// --------------------------------------------------------------- physics ----
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
  int32_t px, py, vx, vy;
  uint8_t edge, cand, candTicks;
  bool grounded;
  uint8_t mode;
  uint8_t startleCd, flapPhase, shakeHits, gyroRun;
  uint16_t dizzyTicks;
  int32_t gfx, gfy, gfz;
  int32_t sx, sy;
  bool wantStartle, wantTumble;
} B = {60 << 8, 40 << 8, 0, 0, EDGE_BOTTOM, EDGE_BOTTOM, 0, false,
       M_GROUND, 0, 0, 0, 0, 0, 0, 1000, 0, 1, 1, false, false};

static int bodyW = 80, bodyH = 65;
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
      // measured on this unit: -ax = screen-right, ay = screen-down (rot 1)
      sxx += (int32_t)(fx * -1000);
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
  if (millis() - dbgT > 5000) {
    dbgT = millis();
    Serial.printf("dbg acc raw ax=%ld ay=%ld az=%ld -> gf=(%ld,%ld) edge=%d\n",
                  (long)dbgAx, (long)dbgAy, (long)dbgAz,
                  (long)B.gfx, (long)B.gfy, B.edge);
  }
  // measured on this unit: -ax = screen-right, ay = screen-down (rot 1)
  int32_t rx = (int32_t)(fx * -1000);
  int32_t ry = (int32_t)(fy * 1000);
  int32_t rz = (int32_t)(fz * 1000);
  B.gfx += (rx - B.gfx) >> 3;
  B.gfy += (ry - B.gfy) >> 3;
  B.gfz += (rz - B.gfz) >> 3;
  int32_t hp = abs(rx - B.gfx) + abs(ry - B.gfy) + abs(rz - B.gfz);
  if (hp > SHAKE_MG) B.shakeHits = min(B.shakeHits + 2, 12);
  else if (B.shakeHits) B.shakeHits--;
  if (B.shakeHits >= 6 && !B.startleCd && B.mode != M_FROZEN && B.mode != M_TUMBLE) {
    B.wantStartle = true;
    B.shakeHits = 0;
    B.startleCd = STARTLE_CD_T;
  }
  if (B.startleCd) B.startleCd--;
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
      if (B.mode == M_TUMBLE) B.mode = M_GROUND;
    }
  }
  B.grounded = onFloor;

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
        B.vx += (int32_t)(esp_random() & 255) - 128;
        B.vy += (int32_t)(esp_random() & 255) - 128;
      }
      Serial.printf("dbg floor -> %d\n", B.edge);
    }
  } else {
    B.candTicks = 0;
  }
}

// ---------------------------------------------------------------- meters ----
// E energy, H ham (appetite for attention), G grump (accumulated indignity)
static uint8_t mE = 85, mH = 25, mG = 0;
static uint32_t lastProtoEvent = 0;
static uint32_t tickCount = 0;
static bool costumeCheckDone = false;
static bool reunionPending = false;
static uint32_t quietUntil = 0;      // sad-song aftermath: no bouncing

static uint8_t clamp100(int v) { return (uint8_t)constrain(v, 0, 100); }
#define ME_ADD(d) mE = clamp100((int)mE + (d))
#define MH_ADD(d) mH = clamp100((int)mH + (d))
#if SHOOT_MODE
#define MG_ADD(d) do {} while (0)          // filming: indignity never accumulates
#else
#define MG_ADD(d) mG = clamp100((int)mG + (d))
#endif

static bool dragonSleepy() {
  static int batt = 100;
  static uint32_t nextPoll = 0;
  if (millis() > nextPoll) { nextPoll = millis() + 30000; batt = M5.Power.getBatteryLevel(); }
  return (batt >= 0 && batt < 20) || mE < 15;
}
static bool dragonStarved() { return millis() - lastProtoEvent > 300000; }
static int animDiv() { return (mE >= 60) ? 2 : (mE >= 25) ? 3 : 4; }

// ------------------------------------------------------------------ life ----
enum Life { L_HATCH, L_PERCH, L_WANDER, L_FLY, L_WATCH,
            L_CAVE_NAP, L_PEEK, L_STARTLE, L_TUMBLE, L_FLOURISH, L_DIZZY,
            L_YAWNWALK, L_DEEPSLEEP, L_PRENAP, L_WAKE, L_GRUMP, L_HATBACK, L_BIT };
static uint8_t life = L_HATCH;
static bool moonwalk = false;
static uint32_t lifeT0 = 0, lifeDwell = 2600;
static uint32_t lastActivity2 = 0;
static int walkDir = 1;
static char errReason[20] = "";
static uint8_t savedLife = L_PERCH;
static uint8_t bitId = 0;
static uint8_t grumpVariant = 0;     // 0 tap-foot, 1 sulk, 2 drama-faint
static uint8_t flourishMood = MOOD_PLAIN;
static bool encore = false;

static void metersTick() {           // once per second
  switch (life) {
    case L_DEEPSLEEP: ME_ADD(3); break;
    case L_CAVE_NAP:  ME_ADD(2); break;
    case L_PEEK:      ME_ADD(1); break;
    case L_PERCH:     if ((tickCount / 20) & 1) ME_ADD(1); break;
    case L_WATCH:     MH_ADD(2); break;
    case L_WANDER:    if ((tickCount / 20) & 1) ME_ADD(-1); break;
    case L_FLY: case L_FLOURISH: case L_BIT: ME_ADD(-2); break;
    case L_GRUMP:     ME_ADD(-1); break;
    default: break;
  }
  if (mG) MG_ADD(-1);
  if (dragonStarved()) mH = max(mH, (uint8_t)70);
  else if ((tickCount % 300) == 0) MH_ADD(1);   // +1 per 15s of quiet
}

static void lifeEnter(uint8_t s, uint32_t mn, uint32_t mx) {
  life = s;
  lifeT0 = millis();
  lifeDwell = mn + (mx > mn ? esp_random() % (mx - mn) : 0);
}

static void lifePick() {
  uint32_t r = esp_random() % 100;
  bool slp = dragonSleepy();
  switch (life) {
    case L_PERCH: {
      if (mG >= 70) { grumpVariant = (mG >= 90) ? 2 : 1; lifeEnter(L_GRUMP, 2000 + mG * 40, 2001 + mG * 40); return; }
      if (!costumeCheckDone && millis() > 600000) {
        costumeCheckDone = true; savedLife = L_PERCH; bitId = 2;
        lifeEnter(L_BIT, 4200, 4201); return;
      }
      if (mE >= 70 && (esp_random() & 31) == 0) {
        savedLife = L_PERCH; bitId = 1; lifeEnter(L_BIT, 1200, 1201); return;
      }
      uint32_t t1 = 15 + mE / 4;
      uint32_t t2 = t1 + (slp ? 0 : (mE + mH) / 8);
      uint32_t t3 = t2 + 12 + mH / 3 + (dragonStarved() ? 20 : 0);
      uint32_t t4 = t3 + (100 - mE) / 4 + (slp ? 25 : 0);
      if (r < t1) { lifeEnter(L_WANDER, 4000, 9000); moonwalk = (esp_random() & 1); }
      else if (r < t2) lifeEnter(L_FLY, 5000, 9000);
      else if (r < t3) lifeEnter(L_WATCH, 2000, 5000);
      else if (r < t4) lifeEnter(L_PRENAP, 1600, 1601);
      else lifeEnter(L_PERCH, 3000, 7000);
      break;
    }
    case L_WANDER:
      if ((esp_random() & 15) == 0) { savedLife = L_PERCH; bitId = 0; lifeEnter(L_BIT, 700, 701); return; }
      if (r < 35) lifeEnter(L_PERCH, 3000, 7000);
      else if (r < 35 + (slp ? 0 : (mE + mH) / 10)) lifeEnter(L_FLY, 5000, 9000);
      else if (r < 60 + (100 - mE) / 5) lifeEnter(L_PRENAP, 1600, 1601);
      else lifeEnter(L_WATCH, 2000, 5000);
      break;
    case L_FLY:
      if (r < 55) lifeEnter(L_PERCH, 3000, 7000);
      else if (r < 85) { lifeEnter(L_WANDER, 4000, 9000); moonwalk = (esp_random() & 1); }
      else lifeEnter(L_WATCH, 2000, 5000);
      break;
    case L_WATCH:
      if (r < 70) lifeEnter(L_PERCH, 3000, 7000);
      else if (r < 85) { lifeEnter(L_WANDER, 4000, 9000); moonwalk = (esp_random() & 1); }
      else lifeEnter(L_WATCH, 2000, mH >= 80 ? 10000 : 5000);
      break;
    case L_PEEK:
      if ((esp_random() & 7) == 0) { savedLife = L_CAVE_NAP; bitId = 3; lifeEnter(L_BIT, 4000, 4001); return; }
      if (r < 70) lifeEnter(L_WAKE, 3500, 3501);
      else lifeEnter(L_CAVE_NAP, 8000, 20000 + (uint32_t)(100 - mE) * 250);
      break;
    default:
      lifeEnter(L_PERCH, 3000, 7000);
  }
}

// ---------------------------------------------------------------- drawing ----
static const uint16_t C_CAVE = 0x632C, C_CAVE_D = 0x2104;

static void drawCave(M5Canvas &c) {
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

static void caveSpot(int *x, int *y) {
  switch (B.edge) {
    case EDGE_BOTTOM: *x = 4;   *y = 135 - 55; break;
    case EDGE_TOP:    *x = 182; *y = 0; break;
    case EDGE_LEFT:   *x = 0;   *y = 4; break;
    default:          *x = 240 - 60; *y = 135 - 66; break;
  }
}

static void drawBatteryIfLow(M5Canvas &c) {
  if (dragonSleepy()) {
    c.drawRect(222, 4, 14, 7, TFT_WHITE);
    c.fillRect(236, 6, 2, 3, TFT_WHITE);
  }
}

static void sparkles(M5Canvas &c, int n) {
  for (int i = 0; i < n; i++)
    c.fillRect(esp_random() % 234, esp_random() % 129, 3, 3,
               (esp_random() & 1) ? TFT_WHITE : 0xFF08);
}

static const DSprite *poseSprite(bool *pFlip) {
  uint32_t now = millis();
  *pFlip = (walkDir < 0);
  if (B.mode == M_TUMBLE) return &SPR_STARTLED;
  if (B.dizzyTicks) return &SPR_STARTLED;
  int div = 90 * animDiv();
  switch (life) {
    case L_WANDER:
      *pFlip = moonwalk ? (walkDir > 0) : (walkDir < 0);
      return ((now / div) & 1) ? &SPR_WALK_A : &SPR_WALK_B;
    case L_YAWNWALK:
      return ((now / (div * 2)) & 1) ? &SPR_WALK_A : &SPR_WALK_B;
    case L_FLY: {
      static const DSprite *dn[4] = {&SPR_SPIN_A, &SPR_SPIN_B, &SPR_KICK, &SPR_SPIN_A};
      *pFlip = (walkDir < 0);
      return dn[(now / 220) & 3];
    }
    case L_STARTLE: return &SPR_STARTLED;
    default: return nullptr;
  }
}

// ------------------------------------------------------- protocol hooks ----
static uint32_t lastFrame2 = 0;
static uint8_t jaw = 0;
static uint8_t wideRun = 0;
static uint32_t lastListenDraw = 0;
static uint16_t eqH[3] = {0, 0, 0};
static bool wasSing = false, wasErr = false;

static void renderScene();

static void drawSingBackdrop() {
  scene.fillSprite(TFT_BLACK);
  switch (dragonMood) {
    case MOOD_UPBEAT:
      scene.fillTriangle(0, 0, 60, 0, 90, 40, 0xC618);
      scene.fillTriangle(240, 0, 180, 0, 150, 40, 0xC618);
      for (int i = 0; i < 10; i++) {
        int x = esp_random() % 236, y = esp_random() % 130;
        if (x < 55 || x > 185 || y < 15 || y > 120) scene.fillRect(x, y, 2, 2, TFT_WHITE);
      }
      break;
    case MOOD_SAD:
      scene.fillSprite(0x000B);
      scene.fillRect(100, 0, 40, 135, 0x2124);
      scene.fillCircle(215, 20, 10, 0xC618);
      break;
    case MOOD_SLEEPY:
      scene.fillRect(0, 110, 240, 3, 0xC300);
      drawSpr(scene, SPR_PROP_ZZ, 12, 8, 2, false, EDGE_BOTTOM);
      drawSpr(scene, SPR_PROP_ZZ, 26, 18, 2, false, EDGE_BOTTOM);
      break;
    default:
      sparkles(scene, 3);
  }
  scene.pushSprite(0, 0);
}

static void faceSetState(FaceState s) {
  if (faceState == FACE_SING && s == FACE_IDLE) wasSing = true;
  if (s == FACE_LISTEN || s == FACE_THINK || s == FACE_SING || s == FACE_ERROR)
    lastProtoEvent = millis();
  faceState = s;
  faceStateT0 = millis();
  lastActivity2 = millis();
  Serial.printf("dbg face -> %s\n", FACE_NAMES2[s]);
  M5.Display.setBrightness(200);
  if (s == FACE_IDLE) {
    if (wasSing) {
      wasSing = false;
      flourishMood = dragonMood;
      dragonMood = MOOD_PLAIN;
      mG = 0;                                    // applause cures everything
      encore = (mH >= 80 && (esp_random() & 7) == 0);
      switch (flourishMood) {                    // aftermath deltas
        case MOOD_UPBEAT: MH_ADD(-45); ME_ADD(-4); break;
        case MOOD_SAD:    MH_ADD(-20); ME_ADD(-2); break;
        case MOOD_SLEEPY: MH_ADD(-30); ME_ADD(-8); break;
        default:          MH_ADD(-35); ME_ADD(-3); break;
      }
      savedLife = L_WATCH;
      lifeEnter(L_FLOURISH, encore ? 3800 : 2600, encore ? 3801 : 2601);
    } else if (wasErr) {
      wasErr = false;
      lifeEnter(L_DIZZY, 2500, 2501);
    } else if (reunionPending) {
      reunionPending = false;
      savedLife = L_PERCH;
      bitId = 4;
      lifeEnter(L_BIT, 1500, 1501);
    } else if (life == L_DEEPSLEEP || life == L_CAVE_NAP) {
      lifeEnter(L_PERCH, 3000, 7000);
    }
    B.mode = (life == L_FLY) ? M_FLY : M_GROUND;
  } else if (s == FACE_SING) {
    M5.Speaker.stop();                           // audio guard: clean slate
    B.mode = M_FROZEN;
    jaw = 0; wideRun = 0; lastSingSpr = nullptr;
    if (dSpritesOk) drawSingBackdrop();          // once, pre-slurp
  } else if (s == FACE_LISTEN) {
    if (dragonStarved()) reunionPending = true;
    MH_ADD(8);
    B.mode = M_FROZEN;
  } else if (s == FACE_THINK) {
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
  drawSpr(scene, SPR_SING_CLOSED, 30, 20, 6, false, EDGE_BOTTOM);
  for (int i = 0; i < 3; i++)
    scene.fillRect(174 + i * 14, 110 - eqH[i], 10, max((int)eqH[i], 3), 0x07FF);
  scene.fillCircle(224, 14, ((now / 400) & 1) ? 8 : 6, 0xF800);
  scene.pushSprite(0, 0);
}

static void drawGroundedBody(const DSprite &spr, int sc, int *outX, int *outY) {
  int w = sprW(spr, sc, B.edge), h = sprH(spr, sc, B.edge);
  int ix, iy;
  switch (B.edge) {
    case EDGE_BOTTOM: ix = (240 - w) / 2; iy = 135 - h; break;
    case EDGE_TOP:    ix = (240 - w) / 2; iy = 0; break;
    case EDGE_LEFT:   ix = 0; iy = (135 - h) / 2; break;
    default:          ix = 240 - w; iy = (135 - h) / 2; break;
  }
  drawSpr(scene, spr, ix, iy, sc, false, B.edge);
  if (outX) *outX = ix;
  if (outY) *outY = iy;
}

static void renderScene() {
  scene.fillSprite(TFT_BLACK);
  uint32_t now = millis();
  uint32_t el = now - lifeT0;
  int px = B.px >> 8, py = B.py >> 8;

  if (life == L_CAVE_NAP || life == L_DEEPSLEEP) {
    drawCave(scene);
    int cx, cy;
    caveSpot(&cx, &cy);
    drawSpr(scene, ((now / 1000) & 1) ? SPR_SLEEP_A : SPR_SLEEP_B, cx, cy + 4, 4, false, B.edge);
    if ((now / 900) & 1) drawSpr(scene, SPR_PROP_ZZ, cx + 60, max(0, cy - 14), 3, false, EDGE_BOTTOM);
  } else if (life == L_PEEK) {
    drawCave(scene);
    int cx, cy;
    caveSpot(&cx, &cy);
    drawSpr(scene, ((now / 400) & 1) ? SPR_WALK_A : SPR_WALK_B, cx + 20, cy + 10, 4,
            B.edge == EDGE_TOP || B.edge == EDGE_RIGHT, B.edge);
  } else if (life == L_BIT && bitId == 3) {
    // sleep-sing: dreaming of the stage — the Zz become music notes
    drawCave(scene);
    int cx, cy;
    caveSpot(&cx, &cy);
    drawSpr(scene, ((now / 1000) & 1) ? SPR_SLEEP_A : SPR_SLEEP_B, cx, cy + 4, 4, false, B.edge);
    drawSpr(scene, SPR_PROP_NOTE, cx + 56 + ((el / 300) % 3) * 8,
            max(0, cy - 10 - (int)((el / 200) % 12)), 2, false, EDGE_BOTTOM);
  } else if (life == L_HATCH) {
    int r = min((int)(el / 18), 78);
    scene.fillCircle(120, 96, r, 0x39C7);
    scene.fillCircle(120, 96, max(0, r - 10), 0x7BCF);
    if (el > 900)
      drawSpr(scene, ((now / 500) & 1) ? SPR_IDLE_A : SPR_IDLE_B, 65, 135 - 90, 5, false, EDGE_BOTTOM);
    if (el > 1400) sparkles(scene, 4);
  } else if (life == L_WAKE) {
    // wake ritual: stir -> sit & rub eyes -> stand bare-headed ->
    // hat flips from the bench onto his head -> proud breath
    drawCave(scene);
    int cx, cy;
    caveSpot(&cx, &cy);
    if (el < 700) {
      drawSpr(scene, (el < 400 || ((now / 150) & 1)) ? SPR_SLEEP_A : SPR_SLEEP_B, cx, cy + 4, 4, false, B.edge);
    } else if (el < 1600) {
      drawSpr(scene, SPR_SIT_RUB, cx + 8, cy + 8, 4, ((el / 300) & 1), EDGE_BOTTOM);
      drawHatAt(scene, cx + 56, cy + 30, 4, EDGE_BOTTOM);
      if (el > 1300 && el < 1360) M5.Speaker.tone(220, 40);
    } else if (el < 2100) {
      int bx = cx + 16, by = cy + 8;
      drawSpr(scene, ((el / 250) & 1) ? SPR_WALK_A : SPR_WALK_B, bx, by, 4, false, EDGE_BOTTOM);
      hatEctomy(scene, bx, by, 4, false, TFT_BLACK);
      drawHatAt(scene, cx + 56, cy + 30, 4, EDGE_BOTTOM);
    } else if (el < 2550) {
      int bx = cx + 16, by = cy + 8;
      drawSpr(scene, SPR_WALK_A, bx, by, 4, false, EDGE_BOTTOM);
      hatEctomy(scene, bx, by, 4, false, TFT_BLACK);
      int hx, hy;
      hatSeat(bx, by, 4, false, &hx, &hy);
      int t = (int)(el - 2100);
      int sxp = cx + 56, syp = cy + 30;
      int mx = (sxp + hx) / 2, my = min(syp, hy) - 16;
      int hx2, hy2;
      uint8_t rot = EDGE_BOTTOM;
      if (t < 150) { hx2 = sxp; hy2 = syp; }
      else if (t < 300) { hx2 = mx; hy2 = my; rot = EDGE_TOP; }
      else { hx2 = hx; hy2 = hy; }
      drawHatAt(scene, hx2, hy2, 4, rot);
      if (t >= 300 && t < 360) { sparkles(scene, 2); M5.Speaker.tone(1200, 30); }
    } else {
      drawSpr(scene, ((now / 300) & 1) ? SPR_IDLE_A : SPR_IDLE_B, cx + 4, max(0, cy - 12), 4, false, EDGE_BOTTOM);
    }
  } else if (life == L_PRENAP) {
    if (el < 800) {
      drawGroundedBody(SPR_LEAN, 5, nullptr, nullptr);
    } else {
      int ix, iy;
      drawGroundedBody(SPR_IDLE_B, 5, &ix, &iy);
      hatEctomy(scene, ix, iy, 5, true, TFT_BLACK);
      int hx, hy;
      hatSeat(ix, iy, 5, true, &hx, &hy);
      drawHatAt(scene, hx, hy + 15, 5, EDGE_BOTTOM);   // brim over the eyes
      drawSpr(scene, SPR_PROP_ZZ, min(ix + 100, 230), max(0, iy - 14), 2, false, EDGE_BOTTOM);
    }
  } else if (life == L_GRUMP) {
    if (grumpVariant == 2 && el < 2000) {
      drawSpr(scene, SPR_STARTLED, 60, 60, 5, false, EDGE_RIGHT);   // drama faint
    } else if (grumpVariant >= 1) {
      int w = sprW(SPR_GRUMPY, 4, B.edge);
      int ix = 240 - w - 4, iy = 135 - sprH(SPR_GRUMPY, 4, B.edge);
      drawSpr(scene, SPR_GRUMPY, ix, iy + (((now / 300) & 1) ? 1 : 0), 4, true, B.edge);
    } else {
      int ix, iy;
      drawGroundedBody(SPR_GRUMPY, 5, &ix, &iy);
      if ((now / 300) & 1) scene.fillRect(ix + 30, min(iy + 84, 131), 15, 4, DPAL[4]);
    }
  } else if (life == L_HATBACK) {
    // tumble aftermath: the hat fell — look, walk over, crouch, flip it back on
    int bx = min(px, 150), by = py;
    int landX = min(bx + 44, 216), landY = 135 - 16;
    if (el < 300) {
      drawSpr(scene, SPR_IDLE_B, bx, by, 4, false, EDGE_BOTTOM);
      hatEctomy(scene, bx, by, 4, true, TFT_BLACK);
      drawHatAt(scene, landX, min((int)(by - 40 + (int)el / 3), landY), 4, EDGE_BOTTOM);
      if (el > 270) M5.Speaker.tone(300, 30);
    } else if (el < 800) {
      drawSpr(scene, SPR_IDLE_B, bx, by, 4, false, EDGE_BOTTOM);
      hatEctomy(scene, bx, by, 4, true, TFT_BLACK);
      drawHatAt(scene, landX, landY, 4, EDGE_BOTTOM);
    } else if (el < 1400) {
      int off = (int)((el - 800) * 20) / 600;
      drawSpr(scene, ((now / 150) & 1) ? SPR_WALK_A : SPR_WALK_B, bx + off, min(by + 8, 83), 4, false, EDGE_BOTTOM);
      hatEctomy(scene, bx + off, min(by + 8, 83), 4, false, TFT_BLACK);
      drawHatAt(scene, landX, landY, 4, EDGE_BOTTOM);
    } else if (el < 1700) {
      drawSpr(scene, SPR_BOW, landX - 40, 135 - 60, 4, false, EDGE_BOTTOM);
      drawHatAt(scene, landX, landY, 4, EDGE_BOTTOM);
    } else {
      int bx2 = landX - 44, by2 = 135 - 76;
      drawSpr(scene, SPR_IDLE_A, bx2, by2, 4, false, EDGE_BOTTOM);
      hatEctomy(scene, bx2, by2, 4, true, TFT_BLACK);
      int hx, hy;
      hatSeat(bx2, by2, 4, true, &hx, &hy);
      int t = (int)(el - 1700);
      int mx = (landX + hx) / 2, my = min(landY, hy) - 16;
      int hx2, hy2;
      uint8_t rot = EDGE_BOTTOM;
      if (t < 150) { hx2 = landX; hy2 = landY; }
      else if (t < 300) { hx2 = mx; hy2 = my; rot = EDGE_TOP; }
      else { hx2 = hx; hy2 = hy; if (t < 360) { sparkles(scene, 2); M5.Speaker.tone(1200, 30); } }
      drawHatAt(scene, hx2, hy2, 4, rot);
    }
  } else if (life == L_BIT) {
    switch (bitId) {
      case 0:
        drawSpr(scene, SPR_SING_WIDE_SQUINT, 90, 30, 3, false, EDGE_BOTTOM);
        drawSpr(scene, SPR_PROP_HEART, 160, 30, 3, false, EDGE_BOTTOM);
        break;
      case 1:
        drawGroundedBody(((now / 160) & 1) ? SPR_KICK : SPR_SPIN_A, 5, nullptr, nullptr);
        if ((el / 400) < 3 && (el % 400) < 60) M5.Speaker.tone(600 + (int)(el / 400) * 200, 50);
        sparkles(scene, 2);
        break;
      case 2:
        if (el < 1000) {
          int ix, iy;
          drawGroundedBody(SPR_IDLE_A, 5, &ix, &iy);
          hatEctomy(scene, ix, iy, 5, true, TFT_BLACK);
          int hx, hy;
          hatSeat(ix, iy, 5, true, &hx, &hy);
          int lift = (el < 500) ? (int)(el / 50) : 10 - (int)((el - 500) / 50);
          drawHatAt(scene, hx + 6, hy - max(0, lift), 5, EDGE_BOTTOM);
        } else if (el < 2200) {
          drawGroundedBody(((now / 110) & 1) ? SPR_SPIN_A : SPR_SPIN_B, 5, nullptr, nullptr);
        } else {
          drawGroundedBody(SPR_LEAN, 6, nullptr, nullptr);
          sparkles(scene, 3);
        }
        break;
      case 4:
        drawGroundedBody(((now / 140) & 1) ? SPR_SPIN_A : SPR_IDLE_A, 5, nullptr, nullptr);
        drawSpr(scene, SPR_PROP_HEART, 60 + (el / 8) % 30, 20, 3, false, EDGE_BOTTOM);
        drawSpr(scene, SPR_PROP_HEART, 150, 30 - (int)((el / 30) % 20), 2, false, EDGE_BOTTOM);
        break;
      default: break;
    }
  } else if (life == L_FLOURISH) {
    switch (flourishMood) {
      case MOOD_UPBEAT:
        if (el < 800) {
          drawSpr(scene, ((now / 100) & 1) ? SPR_SPIN_A : SPR_SPIN_B, 70, 15, 5, false, EDGE_BOTTOM);
          for (int i = 0; i < 4; i++) {
            int a = (int)(8 + el / 25);
            scene.fillRect(constrain(120 + ((i & 1) ? a : -a), 0, 236),
                           constrain(70 + ((i & 2) ? a / 2 : -a / 2), 0, 130), 3, 3, 0xFF08);
          }
        } else if (el < 1400) {
          drawSpr(scene, SPR_KICK, 70, 28, 5, ((el / 300) & 1), EDGE_BOTTOM);
          drawSpr(scene, SPR_PROP_NOTE, min(150 + (int)(el - 800) / 8, 228),
                  max(6, 60 - (int)(el - 800) / 16), 2, false, EDGE_BOTTOM);
        } else if (el < 1900) {
          drawSpr(scene, SPR_LEAN, 70, 40, 6, false, EDGE_BOTTOM);
          sparkles(scene, 3);
        } else {
          drawSpr(scene, SPR_BOW, 75, 45, 5, false, EDGE_BOTTOM);
          drawHatAt(scene, 45, 60, 4, EDGE_BOTTOM);
          drawSpr(scene, SPR_PROP_HEART, 120, max(5, 40 - (int)(el - 1900) / 30), 2, false, EDGE_BOTTOM);
          if (el < 1960) M5.Speaker.tone(523, 50);
        }
        break;
      case MOOD_SAD: {
        int ix, iy;
        if (el < 600) {
          drawGroundedBody(SPR_IDLE_A, 5, &ix, &iy);
          hatEctomy(scene, ix, iy, 5, true, TFT_BLACK);
          int hx, hy; hatSeat(ix, iy, 5, true, &hx, &hy);
          drawHatAt(scene, hx, hy + 15, 5, EDGE_BOTTOM);        // low-hat
          drawSpr(scene, SPR_PROP_NOTE, 95, min(30 + (int)el / 12, 120), 2, false, EDGE_BOTTOM);
        } else if (el < 1400) {
          drawSpr(scene, (el < 1000) ? SPR_SPIN_A : SPR_SPIN_B, 70, 15, 5, false, EDGE_BOTTOM);
        } else if (el < 2200) {
          drawSpr(scene, SPR_BOW, 75, 45, 5, false, EDGE_BOTTOM);
          drawHatAt(scene, 60, 70, 4, EDGE_BOTTOM);              // hat at chest
          if (el < 1460) M5.Speaker.tone(392, 140);
          else if (el >= 1600 && el < 1660) M5.Speaker.tone(330, 140);
        } else {
          drawGroundedBody(SPR_IDLE_A, 5, &ix, &iy);
          hatEctomy(scene, ix, iy, 5, true, TFT_BLACK);
          int hx, hy; hatSeat(ix, iy, 5, true, &hx, &hy);
          drawHatAt(scene, hx, hy + 15, 5, EDGE_BOTTOM);
          drawSpr(scene, SPR_PROP_HEART, 95, 25, 2, false, EDGE_BOTTOM);
        }
        break;
      }
      case MOOD_SLEEPY: {
        int ix, iy;
        drawGroundedBody((el < 600) ? SPR_IDLE_B : SPR_IDLE_A, 5, &ix, &iy);
        if (el >= 600 && el < 1400) {
          hatEctomy(scene, ix, iy, 5, true, TFT_BLACK);
          int hx, hy; hatSeat(ix, iy, 5, true, &hx, &hy);
          int dip = (el < 1000) ? (int)(el - 600) / 27 : 15 - (int)(el - 1000) / 27;
          drawHatAt(scene, hx, hy + max(0, dip), 5, EDGE_BOTTOM);
        }
        if (el >= 1400) drawSpr(scene, SPR_PROP_ZZ, 110, max(0, 20 - (int)(el - 1400) / 40), 2, false, EDGE_BOTTOM);
        break;
      }
      default:
        if (el < 700) {
          drawSpr(scene, ((now / 110) & 1) ? SPR_SPIN_A : SPR_SPIN_B, 70, 15, 5, false, EDGE_BOTTOM);
          sparkles(scene, 4);
        } else if (el < 1200) {
          drawSpr(scene, SPR_LEAN, 70, 40, 6, false, EDGE_BOTTOM);
          sparkles(scene, 3);
        } else if (el < 1700) {
          drawSpr(scene, SPR_IDLE_A, 70, 20, 5, false, EDGE_BOTTOM);
          drawSpr(scene, SPR_PROP_NOTE, 40 + (el / 9) % 160, 20 + ((el / 13) % 40), 3, false, EDGE_BOTTOM);
          sparkles(scene, 3);
        } else if (!encore || el < 2100) {
          drawSpr(scene, SPR_IDLE_B, 70, 20, 5, false, EDGE_BOTTOM);
          hatEctomy(scene, 70, 20, 5, true, TFT_BLACK);
          int hx, hy; hatSeat(70, 20, 5, true, &hx, &hy);
          int lift = (el < 1900) ? (int)(el - 1700) / 18 : 11 - (int)min((uint32_t)11, (el - 1900) / 40);
          drawHatAt(scene, hx + 6, hy - max(0, lift), 5, EDGE_BOTTOM);
          if (el >= 1700 && el < 1760) M5.Speaker.tone(660, 50);
        } else {
          // ENCORE: bow + hat toss
          drawSpr(scene, SPR_BOW, 75, 45, 5, false, EDGE_BOTTOM);
          int t = (int)((el - 2100) % 900);
          int hy2 = (t < 450) ? 60 - t / 15 : 30 + (t - 450) / 15;
          drawHatAt(scene, 55, hy2, 4, (t > 220 && t < 680) ? EDGE_TOP : EDGE_BOTTOM);
          sparkles(scene, 3);
        }
    }
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
    uint32_t tel = now - faceStateT0;
    if (dragonSleepy() && tel < 1400) {
      drawSpr(scene, SPR_SIT_RUB, 80, 55, 5, ((tel / 300) & 1), EDGE_BOTTOM);
      if (tel > 900 && tel < 1000) drawSpr(scene, SPR_PROP_ZZ, 150, 30, 2, false, EDGE_BOTTOM);
    } else if (tel < 2000) {
      const DSprite *w = &SPR_IDLE_A;
      if (tel < 400) w = (tel < 200) ? &SPR_IDLE_A : &SPR_IDLE_B;
      else if (tel < 550) w = &SPR_IDLE_B;
      else if (tel < 800 && tel >= 700) w = &SPR_SPIN_A;
      drawSpr(scene, *w, 65, 22 + ((tel >= 400 && tel < 550) ? 3 : 0), 5, false, EDGE_BOTTOM);
      if (tel >= 400 && tel < 460) M5.Speaker.tone(180, 40);
      if (tel >= 1100) {
        static const int freqs[3] = {523, 659, 784};
        int i = (int)((tel - 1100) / 200);
        if (i < 3 && ((tel - 1100) % 200) < 60) M5.Speaker.tone(freqs[i], 70);
        drawSpr(scene, SPR_PROP_NOTE, 175, max(6, 40 - (int)(tel - 1100) / 30), 2, false, EDGE_BOTTOM);
      }
      if (tel >= 1700) {
        hatEctomy(scene, 65, 22, 5, true, TFT_BLACK);
        int hx, hy; hatSeat(65, 22, 5, true, &hx, &hy);
        drawHatAt(scene, hx, hy - 8, 5, EDGE_BOTTOM);
      }
    } else {
      int bob = ((now / 300) & 1) ? 1 : 0;
      drawSpr(scene, ((now / 500) & 1) ? SPR_IDLE_A : SPR_IDLE_B, 65, 22 + bob, 5, false, EDGE_BOTTOM);
    }
    scene.setTextDatum(top_center);
    scene.setTextColor(TFT_WHITE, TFT_BLACK);
    scene.drawString(((now / 800) & 1) ? "?" : "...", 130, 8);
  } else if (life == L_WATCH) {
    uint32_t wel = now - lifeT0;
    if (wel < 900) {
      int bx = 76, by = 38;
      drawSpr(scene, (wel < 300) ? SPR_IDLE_A : SPR_IDLE_B, bx, by, 4, false, EDGE_BOTTOM);
      if (wel >= 300) {
        hatEctomy(scene, bx, by, 4, true, TFT_BLACK);
        int hx, hy; hatSeat(bx, by, 4, true, &hx, &hy);
        int lift = (wel < 600) ? (int)(wel - 300) / 25 : 12 - (int)(wel - 600) / 25;
        drawHatAt(scene, hx + 8, hy - max(0, lift), 4, EDGE_BOTTOM);
        if (wel >= 450 && wel < 500) M5.Speaker.tone(988, 40);
      }
    } else {
      drawSpr(scene, SPR_SING_CLOSED, 45, 12, 7, false, EDGE_BOTTOM);
      if (dragonStarved() && wel < 1600 && (wel / 300) & 1) M5.Speaker.tone(700, 25);
    }
  } else {
    drawCave(scene);
    bool flip;
    const DSprite *s = poseSprite(&flip);
    if (s) {
      drawSpr(scene, *s, px, py, 5, flip, B.edge);
    } else {
      const DSprite &idle = (life == L_STARTLE) ? SPR_STARTLED
                            : ((quietUntil > now) ? SPR_IDLE_A
                               : (((now / (250 * animDiv())) & 1) ? SPR_IDLE_A : SPR_IDLE_B));
      int ix, iy;
      drawGroundedBody(idle, 5, &ix, &iy);
      if (life == L_STARTLE) {
        scene.setTextDatum(top_center);
        scene.setTextColor(TFT_WHITE, TFT_BLACK);
        scene.drawString("!", ix + sprW(idle, 5, B.edge) / 2, max(0, iy - 14));
      }
      if (dragonSleepy() && ((now / 3000) % 3 == 0))
        drawSpr(scene, SPR_PROP_ZZ, min(ix + sprW(idle, 5, B.edge) - 8, 225), max(0, iy - 16), 2, false, EDGE_BOTTOM);
    }
  }
  drawBatteryIfLow(scene);
  scene.fillCircle(8, 10, 4, dragonNetUp ? 0x34DF : 0x7BEF);
  scene.pushSprite(0, 0);
}

static void faceTick() {
  uint32_t now = millis();
  if (faceState == FACE_SING || faceState == FACE_LISTEN) return;
  if (now - lastFrame2 < ((life == L_DEEPSLEEP) ? 200 : 50)) return;
  lastFrame2 = now;
  tickCount++;
  if (tickCount % 20 == 0) metersTick();

  if (faceState == FACE_BOOT) {
    if (now - faceStateT0 > 400) faceSetState(FACE_IDLE);
    return;
  }
  if (faceState == FACE_THINK) {
    physSample();
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

  // ---- FACE_IDLE: the star lives ----
  physTick();

  if (B.wantStartle) {
    B.wantStartle = false;
    lastActivity2 = now;
    if (life == L_DEEPSLEEP || life == L_CAVE_NAP || life == L_PEEK) {
      // picked up while napping: wake ritual (stir, rub eyes, hat flips on)
      M5.Display.setBrightness(200);
      B.mode = M_GROUND;
      lifeEnter(L_WAKE, 3500, 3501);
    } else {
      MG_ADD(30);
      M5.Speaker.tone(880, 40);
      lifeEnter(L_STARTLE, 1000, 1001);
    }
  }
  if (B.wantTumble) {
    B.wantTumble = false;
    lastActivity2 = now;
    MG_ADD(12);
    if (life != L_STARTLE) lifeEnter(L_TUMBLE, 3000, 3001);
  }

  if (now - lastActivity2 > 45000 && life != L_DEEPSLEEP && life != L_YAWNWALK
      && life != L_CAVE_NAP && life != L_PRENAP) {
    lifeEnter(L_YAWNWALK, 6000, 6001);
  }

  switch (life) {
    case L_HATCH:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_PERCH, 3000, 7000);
      break;
    case L_WANDER: case L_YAWNWALK: {
      int32_t *vt = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? &B.vy : &B.vx;
      int v = (life == L_YAWNWALK || dragonSleepy() || mE < 60) ? WALK_V / 2 : WALK_V;
      if (life == L_YAWNWALK) {
        int cx, cy;
        caveSpot(&cx, &cy);
        int32_t target = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? ((int32_t)cy << 8) : ((int32_t)cx << 8);
        int32_t pos = (B.edge == EDGE_LEFT || B.edge == EDGE_RIGHT) ? B.py : B.px;
        walkDir = (target > pos) ? 1 : -1;
        if (abs(target - pos) < (6 << 8)) { lifeEnter(L_DEEPSLEEP, 3600000, 3600001); M5.Display.setBrightness(90); break; }
      } else if ((esp_random() & 63) == 0) {
        walkDir = -walkDir;
      }
      if (B.grounded) *vt = walkDir * v;
      if ((B.px >> 8) <= 1 || (B.px >> 8) >= 238 - bodyW) walkDir = -walkDir;
      if (now - lifeT0 > lifeDwell && life == L_WANDER) lifePick();
      break;
    }
    case L_FLY:
      if (now - lifeT0 > lifeDwell) lifePick();
      break;
    case L_STARTLE:
      if (now - lifeT0 > lifeDwell) {
        if (B.mode == M_TUMBLE) lifeEnter(L_TUMBLE, 3000, 3001);
        else if (mG >= 40) { grumpVariant = (mG >= 90) ? 2 : (mG >= 70) ? 1 : 0; lifeEnter(L_GRUMP, 2000 + (uint32_t)mG * 40, 2001 + (uint32_t)mG * 40); }
        else lifeEnter(L_PERCH, 3000, 7000);
      }
      break;
    case L_TUMBLE:
      if (B.mode == M_GROUND && B.grounded) {
        if (B.edge == EDGE_BOTTOM) lifeEnter(L_HATBACK, 2200, 2201);
        else if (mG >= 40) { grumpVariant = (mG >= 70) ? 1 : 0; lifeEnter(L_GRUMP, 2000 + (uint32_t)mG * 40, 2001 + (uint32_t)mG * 40); }
        else lifeEnter(L_PERCH, 3000, 7000);
      } else if (now - lifeT0 > 4000) {
        B.mode = M_GROUND;
        lifeEnter(L_PERCH, 3000, 7000);
      }
      break;
    case L_HATBACK:
      if (now - lifeT0 > lifeDwell) {
        if (mG >= 40) { grumpVariant = (mG >= 70) ? 1 : 0; lifeEnter(L_GRUMP, 2000 + (uint32_t)mG * 40, 2001 + (uint32_t)mG * 40); }
        else lifeEnter(L_PERCH, 3000, 7000);
      }
      break;
    case L_GRUMP:
      if (now - lifeT0 > lifeDwell) {
        MG_ADD(-30);
        lifeEnter(L_PERCH, 3000, 7000);
      }
      break;
    case L_FLOURISH:
      if (now - lifeT0 > lifeDwell) {
        if (flourishMood == MOOD_SAD) { quietUntil = now + 8000; lifeEnter(L_PERCH, 8000, 8001); }
        else lifeEnter(L_WATCH, 2000, 4000);
      }
      break;
    case L_DIZZY:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_PERCH, 3000, 7000);
      break;
    case L_PRENAP:
      if (now - lifeT0 > lifeDwell) lifeEnter(L_YAWNWALK, 6000, 6001);
      break;
    case L_WAKE:
      if (now - lifeT0 > lifeDwell) { mG = 0; lifeEnter(L_PERCH, 3000, 7000); }
      break;
    case L_BIT:
      if (now - lifeT0 > lifeDwell) {
        MH_ADD(-15);
        if (savedLife == L_CAVE_NAP) lifeEnter(L_CAVE_NAP, 8000, 20000);
        else lifeEnter(L_PERCH, 3000, 7000);
      }
      break;
    case L_DEEPSLEEP: {
      static const uint8_t br[8] = {60, 80, 105, 120, 105, 80, 60, 50};
      M5.Display.setBrightness(br[(now / 400) & 7]);
      renderScene();
      return;
    }
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
  lastProtoEvent = millis();
  lifeEnter(L_HATCH, 2600, 2601);
}
