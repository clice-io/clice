/// # Expand auto in declarations
///
/// - status: supported
///
/// `auto` in a variable declaration is replaced by the type it deduced, leaving qualifiers and declarators in place

namespace ns {
struct Point { struct Delta {}; };
Point::Delta make();
}

void f() {
    §(plain)auto x = 1;
    const §(ref)auto& y = x;
    §(pointer)auto* z = &x;
    §(nested)auto d = ns::make();
}
