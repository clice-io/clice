// use-after-move is unreliable on code being edited and const-correctness
// too slow for an editor: of the three checks enabled, only integer division
// runs in the editor. `clice lint` runs them all.
namespace std {
template <class T> T&& move(T& t) { return static_cast<T&&>(t); }
}
struct S { int v; };
int use(S s) { S t = std::move(s); return s.v + t.v; }
double ratio(int a, int b) { return a / b; }
