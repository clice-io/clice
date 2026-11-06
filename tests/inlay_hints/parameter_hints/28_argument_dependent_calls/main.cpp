void g(int global_first, int global_second);

namespace N {
struct S {};
void g(S adl_first, int adl_second);
}  // namespace N

template <class T>
void call(T t) {
    g(t, 1);
}

void use() {
    call(N::S{});
}
