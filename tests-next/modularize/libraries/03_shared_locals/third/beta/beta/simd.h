#pragma once
#include <csimd.h>
#include <cvalue.h>
inline int beta_simd(int v) { return fake_simd(v) + fake_noise(fake_deep(fake_value)); }
