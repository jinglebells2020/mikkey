// Shared face types — in a header so Arduino's auto-generated function
// prototypes (inserted after #includes, before sketch code) can see them.
#pragma once
#include <stdint.h>

enum FaceState { FACE_BOOT, FACE_IDLE, FACE_LISTEN, FACE_THINK, FACE_SING, FACE_ERROR };

struct EyeParams {
  int8_t pdx, pdy;   // pupil offset, px
  uint8_t lidPct;    // 0=open .. 100=closed
  uint8_t style;     // 0 normal, 1 squint, 2 happy-arc, 3 dim(error)
};
