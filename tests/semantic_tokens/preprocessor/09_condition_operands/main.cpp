#define FEATURE 1

#ifdef FEATURE
int with_feature = 1;
#endif

#ifndef MISSING
int without_missing = 2;
#endif

#if defined(FEATURE) && !defined(MISSING) && __has_include("main.cpp")
int all_three = 3;
#endif

#undef FEATURE

#if __has_include(<missing_header.h>)
int with_missing_header = 4;
#endif
