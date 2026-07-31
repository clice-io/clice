/// # Conflict & Ambiguity
///
/// ## Mixed-kind names — a name naming entities of different kinds renders as `conflict`
///
/// - status: supported
/// - order: 1

namespace shop {
struct §Widget {};
void §Widget();
}

using shop::§Widget;
