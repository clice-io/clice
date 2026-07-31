/// # Conflict & Ambiguity
///
/// ## Type vs function — a name naming both renders as `conflict`
///
/// - status: supported
/// - order: 1

namespace shop {
struct §Widget {};
void §Widget();
}

using shop::§Widget;
