#pragma once
#include <cvalue.h>
#include <cwrap.h>
static int alpha_unused(int v) { return fake_noise(v); }
inline int alpha_simd(int v) { return fake_simd2(v) + fake_wrap(fake_value); }
