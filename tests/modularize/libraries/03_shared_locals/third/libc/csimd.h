#pragma once
static inline int fake_simd(int v) { return v; }
static inline int fake_simd2(int v) { return fake_simd(v) * 2; }
static inline int fake_noise(int v) { return v; }
static inline int fake_deep(int v) { return v; }
