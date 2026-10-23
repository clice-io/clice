#ifndef SIZES_H
#define SIZES_H

#define SIZES_MAX 64
#define SIZES_CLAMP(value) ((value) < SIZES_MAX ? (value) : SIZES_MAX)

#define SIZES_TWICE(value) ((value) * 2)
inline constexpr int sizes_default = SIZES_TWICE(SIZES_MAX);
#undef SIZES_TWICE

#endif
