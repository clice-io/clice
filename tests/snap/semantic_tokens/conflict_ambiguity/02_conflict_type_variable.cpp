/// # Type vs variable
///
/// - status: supported
///
/// A name naming both renders as `conflict`

namespace mixed {
struct Thing {};
int Thing;
}

using mixed::§Thing;
