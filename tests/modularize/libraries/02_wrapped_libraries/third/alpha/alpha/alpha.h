#pragma once
#include <fakecstdio>
#include <fassert.h>
#define ALPHA_VERSION 3
#define ALPHA_TWICE(x) ((x) * 2)
#define ALPHA_EXTERN extern
#ifdef ALPHA_WIDE
#endif
#ifdef ALPHA_TMP
#endif
namespace alpha {
struct Thing {
    int v;
    friend bool operator==(Thing, Thing) { return true; }
};
#if defined(TWO) || defined(THREE)
enum Color { blue };
namespace wide {}
#else
enum Color { red };
#endif
enum { alpha_low, alpha_high };
int make(int);
using fakestd::fake_abs;
}
#if defined(TWO) || defined(THREE)
namespace al = alpha::wide;
#else
namespace al = alpha;
#endif
