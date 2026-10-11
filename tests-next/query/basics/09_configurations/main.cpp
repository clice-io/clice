#ifdef RELEASE
int release_only() { return 1; }
#else
int debug_only() { return 0; }
#endif
