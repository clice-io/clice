int mix(int x) { return x; }
static int mix(double x) { return 2; }
int use_mix() { return mix(1) + mix(2.0); }
