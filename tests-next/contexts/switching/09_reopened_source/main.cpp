#ifdef USE_B
int broken() { return undefined_b_symbol; }
#endif
int main() { return 0; }
