#pragma once

#define SHAPES_API

#if SHAPES_FAST
inline constexpr int shapes_precision = 1;
#else
inline constexpr int shapes_precision = 3;
#endif

#ifdef SHAPES_TRACE
inline void shapes_trace() {}
#endif
