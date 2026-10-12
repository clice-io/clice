// A conditional nested in an active branch closes on its own `#endif` and
// leaves the enclosing branches intact, `#elif` arms included.

#define OUTER

#ifdef OUTER
#if defined(A)
int a;
#elif defined(B)
int b;
#endif
int outer;
#else
int other;
#endif
