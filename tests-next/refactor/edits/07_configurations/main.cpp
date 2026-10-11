int compute(int x) { return x; }

#ifdef RELEASE
int fast() { return compute(1); }
#else
int checked() { return compute(2); }
#endif
