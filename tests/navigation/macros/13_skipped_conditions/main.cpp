// The conditions after a branch taken are never evaluated: their names
// still reference the definition visible there.
#define §(definition)B 1

#if 1
int taken;
#elifdef §(elifdef)B
int skipped;
#elif §(elif)B
int other;
#endif

#if 0
#ifdef §(nested)B
int nested;
#endif
#endif
