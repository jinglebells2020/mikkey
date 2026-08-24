// Mikkey firmware for M5StickS3 — USB serial edition. No WiFi.
//
// Link: native USB CDC. Host pushes clips as:
//   16-byte header:  "MIKY" + u32 sample_rate + u32 pcm_samples + u32 env_len (LE)
//   env_len bytes:   mouth envelope, one byte per 441-sample frame (50 fps)
//   pcm_samples*2:   raw s16le mono PCM
// Device sends ASCII lines up: READY / TRIG / DONE <n> / UNDERRUN <n> / dbg text.
//
// Audio: M5Unified Speaker (ES8311 codec), 3 rotating 441-sample buffers.
// Mouth: env frame advances per chunk queued -> locked to samples, not clock.

#include <M5Unified.h>

static const uint32_t CHUNK = 441;           // samples per chunk = one env frame (20 ms)
static const size_t   ENV_MAX = 16384;       // ~5.5 min of envelope, way more than any clip
static const size_t   PCM_MAX = 4 * 1024 * 1024;  // PSRAM clip buffer: ~95s of audio

static uint8_t envBuf[ENV_MAX];
static int16_t *clipBuf = nullptr;           // ps_malloc'd in setup

static M5Canvas mouth(&M5.Display);
static int mouthW = 0, mouthH = 0, mouthX = 0, mouthY = 0;
static uint8_t lastDrawn = 255;

static void drawFaceStatic() {
  M5.Display.fillScreen(TFT_BLACK);
  int w = M5.Display.width();
  // eyes
  M5.Display.fillCircle(w / 2 - 45, 32, 13, TFT_WHITE);
  M5.Display.fillCircle(w / 2 + 45, 32, 13, TFT_WHITE);
  M5.Display.fillCircle(w / 2 - 45, 32, 6, TFT_BLACK);
  M5.Display.fillCircle(w / 2 + 45, 32, 6, TFT_BLACK);
}

static void setMouthOpenness(float openness) {  // 0.0 - 1.0
  uint8_t q = (uint8_t)(openness * 100);
  if (q == lastDrawn) return;                   // cheap out if unchanged
  lastDrawn = q;
  mouth.fillSprite(TFT_BLACK);
  int h = 6 + (int)(openness * (mouthH - 10));
  int w = mouthW * 6 / 10 + (int)(openness * mouthW * 3 / 10);
  int x = (mouthW - w) / 2, y = (mouthH - h) / 2;
  mouth.fillRoundRect(x, y, w, h, h / 2 < 8 ? h / 2 : 8, TFT_RED);
  if (h > 20) mouth.fillRoundRect(x + w / 8, y + h / 2, w * 3 / 4, h / 2 - 3, 6, TFT_MAROON);
  mouth.pushSprite(mouthX, mouthY);
}

static void pollButton() {
  M5.update();
  if (M5.BtnA.wasPressed()) Serial.println("TRIG");
}

// Read exactly n bytes from USB serial. Keeps the button alive while waiting.
// timeout_ms of silence -> false.
static bool readExact(uint8_t *dst, size_t n, uint32_t timeout_ms) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < n) {
    size_t avail = Serial.available();
    if (avail > 0) {
      got += Serial.read(dst + got, (avail < n - got) ? avail : n - got);
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
  if (nEnv > ENV_MAX) { Serial.println("dbg env too big, abort"); return; }
  if (!readExact(envBuf, nEnv, 3000)) { Serial.println("dbg env timeout, abort"); return; }

  // Phase 1: slurp the whole clip into PSRAM at full USB speed.
  // Reading flat-out keeps the CDC RX buffer from ever filling (which stalls it).
  if (nSamples * 2 > PCM_MAX) { Serial.println("dbg clip too big, abort"); return; }
  uint32_t t0 = millis();
  if (!readExact((uint8_t *)clipBuf, nSamples * 2, 4000)) {
    Serial.println("dbg pcm slurp timeout, abort");
    Serial.println("DONE 0");
    return;
  }
  Serial.printf("dbg slurped %lu bytes in %lu ms\n",
                (unsigned long)(nSamples * 2), (unsigned long)(millis() - t0));

  // Phase 2: play from PSRAM, one 20 ms chunk per envelope frame.
  uint32_t framesDone = 0, underruns = 0, remaining = nSamples;
  while (remaining > 0) {
    uint32_t take = remaining < CHUNK ? remaining : CHUNK;
    while (M5.Speaker.isPlaying(0) >= 2) { pollButton(); delay(1); }  // wait for a free slot
    if (framesDone > 0 && M5.Speaker.isPlaying(0) == 0) underruns++;
    M5.Speaker.playRaw(clipBuf + framesDone * CHUNK, take, rate, false, 1, 0);

    if (framesDone < nEnv) setMouthOpenness(envBuf[framesDone] / 255.0f);
    framesDone++;
    remaining -= take;
  }
  while (M5.Speaker.isPlaying(0)) { pollButton(); delay(5); }
  setMouthOpenness(0);
  if (underruns) Serial.printf("UNDERRUN %lu\n", (unsigned long)underruns);
  Serial.printf("DONE %lu\n", (unsigned long)framesDone);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Speaker.setVolume(255);
  M5.Display.setRotation(1);  // landscape, 240x135
  drawFaceStatic();

  mouthW = 170; mouthH = 72;
  mouthX = (M5.Display.width() - mouthW) / 2;
  mouthY = 55;
  mouth.setColorDepth(16);
  mouth.createSprite(mouthW, mouthH);
  setMouthOpenness(0.0f);

  clipBuf = (int16_t *)ps_malloc(PCM_MAX);

  Serial.setRxBufferSize(32768);  // big buffer: PCM streams in faster than 44 kB/s
  Serial.begin(115200);           // baud is ignored on native CDC
  delay(300);
  if (!clipBuf) Serial.println("dbg FATAL: ps_malloc failed");
  Serial.printf("READY board=%d psram=%u\n", (int)M5.getBoard(), ESP.getPsramSize());
}

static const uint8_t MAGIC[4] = {'M', 'I', 'K', 'Y'};

void loop() {
  pollButton();
  // hunt for magic byte-by-byte so we can always resync
  static int magicPos = 0;
  while (Serial.available()) {
    uint8_t b;
    Serial.read(&b, 1);
    if (b == MAGIC[magicPos]) {
      if (++magicPos == 4) {
        magicPos = 0;
        uint8_t hdr[12];
        if (!readExact(hdr, 12, 2000)) { Serial.println("dbg header timeout"); return; }
        uint32_t rate, nSamples, nEnv;
        memcpy(&rate, hdr, 4); memcpy(&nSamples, hdr + 4, 4); memcpy(&nEnv, hdr + 8, 4);
        if (rate < 8000 || rate > 48000 || nSamples > 60UL * 48000) {
          Serial.println("dbg bogus header, ignored");
          return;
        }
        playClip(rate, nSamples, nEnv);
      }
    } else {
      magicPos = (b == MAGIC[0]) ? 1 : 0;
    }
  }
  delay(2);
}
