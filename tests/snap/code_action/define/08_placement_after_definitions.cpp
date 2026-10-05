/// # Placement after existing definitions
///
/// - status: supported
///
/// When the class already has out-of-line definitions in the file, the new one goes after the last of them
///
/// The qualifier follows the scope of that definition, not the class's.

namespace ns {
struct S {
    void a();
    void §(b)b();
};
}

void ns::S::a() {
}
