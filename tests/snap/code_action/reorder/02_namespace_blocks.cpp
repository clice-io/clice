/// # Namespace blocks reorder separately
///
/// - status: supported
///
/// Definitions written in different namespace blocks are reordered within each block, never across

namespace ns {
struct §(cls)S {
    void a();
    void b();
    void c();
    void d();
};
}

namespace ns {
void S::b() {
}
void S::a() {
}
}

void ns::S::d() {
}
void ns::S::c() {
}
