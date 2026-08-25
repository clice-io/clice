/// # Type Hierarchy
///
/// ## Template inheritance
///
/// - status: supported
/// - verify: server
/// - order: 4
///
/// Subtypes of a base include classes that derive from it through a class
/// template, such as a CRTP wrapper.

struct §(base)Base {};

template <typename T>
struct CRTP : Base {};

struct Widget : CRTP<Widget> {};
