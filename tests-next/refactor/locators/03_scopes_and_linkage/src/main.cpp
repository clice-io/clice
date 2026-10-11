namespace a { int f(int x) { return x; } }
namespace b { int f(double x) { return 1; } }
struct S { int g(int x); int g(double x); };
int S::g(int x) { return x; }
int S::g(double x) { return 1; }
int use(S s) { return a::f(1) + b::f(2.0) + s.g(1) + s.g(2.0); }
