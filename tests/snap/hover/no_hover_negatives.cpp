/// # Special Hover Targets
///
/// ## No hover on meaningless tokens — builtin keywords and empty bodies yield no card
///
/// - status: supported
/// - order: 8
///
/// Hovering a builtin type keyword or the inside of an empty body
/// produces no card at all, so editors show nothing rather than noise.
/// (Numeric and bool literals also have no card today, but that is a
/// tracked gap — see the numeric-literal item — not a promise.)

namespace negatives {

§(builtin_type)int counter = 0;

void noop() {§(empty_braces)}

}
