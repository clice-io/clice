/// # Type vs function — a name naming both renders as `conflict`
///
/// - status: supported

namespace shop {
struct §Widget {};
void §Widget();
}

using shop::§Widget;
