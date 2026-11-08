#pragma once

#ifndef BUFFER_SIZE
#define BUFFER_SIZE 64
#endif

#ifdef SMALL_BUFFERS
#define LIMIT 16
#else
#define LIMIT 256
#endif

#define TWICE(n) ((n) * 2)
#define CLAMP(n) ((n) < LIMIT ? (n) : LIMIT)
#define APPLY(f, n) f(n)
#define CAT(a, b) a##b
