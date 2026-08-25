/// # Go to Type Definition
///
/// ## Smart pointer to the pointee type
///
/// - status: partial
/// - verify: server
/// - order: 4
/// - issues: clangd#1026
///
/// Go-to-type-definition on a smart-pointer variable reaches the wrapper
/// type itself; unwrapping to the pointee type is not offered.

template <typename T>
struct Ptr {
    T* operator->();
    T& operator*();
    T* raw;
};

struct §(type)Widget {};

int use(Ptr<Widget> §(ptr)ptr) {
    return 0;
}
