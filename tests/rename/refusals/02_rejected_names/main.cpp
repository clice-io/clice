#define TWICE(v) ((v) * 2)

struct Point { int x; };

struct Base { int own; };
struct Derived : Base { int extra; };
struct Sibling : Base { int §(sibling)apart; };

int §(compute)compute(int v) { return TWICE(v); }
double other(double v) { return v; }

int main() { return compute(1) + other(2.0); }
