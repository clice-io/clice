#if !defined(PRODUCT) || !defined(FIRST_HALF) || defined(TESTING)
#error alpha.cpp takes PRODUCT and FIRST_HALF only
#endif
int alpha() { return 0; }
