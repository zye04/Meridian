// The grade LUT's domain, shared by its bake (grade_lut.comp) and the
// tonemap lookup. Must match grade.h.
#ifndef GRADE_GLSL
#define GRADE_GLSL

const uint kGradeLutSize = 65u;
const float kLutMinEv = -12.47393;
const float kLutMaxEv = 8.0;

// AgX's inset (Rec.709 -> AgX working space). The LUT is indexed after it,
// so saturated highlights don't clip early.
const mat3 kRec709ToAgxInset = mat3(
    0.544814816, 0.140416759, 0.088810540,
    0.373786466, 0.754137100, 0.178871522,
    0.081398195, 0.105444695, 0.732317593);

#endif
