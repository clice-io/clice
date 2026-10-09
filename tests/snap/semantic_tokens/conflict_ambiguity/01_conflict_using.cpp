/// # Type vs function
///
/// - status: supported
///
/// A name naming both renders as `conflict`

namespace shop {
struct §Widget {};
void §Widget();
}

using shop::§Widget;
