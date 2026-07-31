/// # Conflict & Ambiguity
///
/// ## Type vs variable — a name naming both renders as `conflict`
///
/// - status: supported
/// - order: 2

namespace mixed {
struct Thing {};
int Thing;
}

using mixed::§Thing;
