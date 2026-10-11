int scale(int x) { return x; }
double scale(double x) { return x; }
template <typename T> T twice(T v) { return scale(v) * 2; }
#define CALL scale(1)
int use() { return scale(1) + twice(2); }
