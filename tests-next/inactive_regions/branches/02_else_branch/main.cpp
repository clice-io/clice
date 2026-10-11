#define USE_A 1
int x();
#if USE_A
int active();
#else
int dead();
#endif
int main() { return 0; }
