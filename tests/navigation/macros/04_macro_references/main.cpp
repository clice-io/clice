#define §(first)FEATURE 1

int on = FEATURE;

#ifdef FEATURE
int guarded = 1;
#endif

#ifndef FEATURE
int missing = 0;
#endif

#undef FEATURE

#define §(second)FEATURE 2

int again = FEATURE;
