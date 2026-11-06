#define FEATURE 0

int base();

#if FEATURE
int feature_on();
#else
int feature_off();
#endif
