/// # Special Hover Targets
///
/// ## No hover on meaningless tokens — builtins, literals and empty braces yield no card
///
/// - status: supported
/// - order: 8
///
/// Hovering a builtin type keyword, a literal or the inside of an empty
/// body produces no card at all, so editors show nothing rather than noise.

namespace negatives {

§(builtin_type)int counter = 0;

auto enabled = tr§(bool_literal)ue;

auto answer = §(int_literal)42;

void noop() {§(empty_braces)}

}
