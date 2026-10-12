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
enum Color { red };
enum { alpha_low, alpha_high };
int make(int);
using fakestd::fake_abs;
}
namespace al = alpha;
