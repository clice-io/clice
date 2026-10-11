#define TWICE(v) ((v) * 2)

struct Point { int x; };

int §(compute)compute(int v) { return TWICE(v); }
double other(double v) { return v; }

int main() { return compute(1) + other(2.0); }
