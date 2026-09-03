/// # Type vs variable — a name naming both renders as `conflict`
///
/// - status: supported

namespace mixed {
struct Thing {};
int Thing;
}

using mixed::§Thing;
