namespace std {
template <class T> T&& move(T& t) { return static_cast<T&&>(t); }
}
struct S { int v; };
int use(S s) { S t = std::move(s); return s.v + t.v; }
double ratio(int a, int b) { return a / b; }
