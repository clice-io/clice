// Once a branch is taken, the preprocessor skips the rest of the chain and
// reports no `#else` behind an `#elif`; every arm still folds, nested
// conditionals in the skipped arms included.

#if 1
int a;
#elif FOO
int b;
#else
int c;
#endif

#ifdef __cplusplus
int d;
#elifndef BAR
int e;
#else
#ifdef BAZ
int f;
int g;
#endif
#endif
