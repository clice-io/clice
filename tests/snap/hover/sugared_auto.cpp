/// # Type Information
///
/// ## Sugared `auto` — alias sugar preserved through deduction
///
/// - status: supported
/// - order: 10
///
/// clangd tracks lost alias sugar through `auto` as clangd#709; clice
/// already keeps the alias spelling and appends its desugared form, so
/// `auto` deduced from an aliased return type reads as `Outer // aka: int`.

namespace sugared_auto {

using Inner = int;
using Outer = Inner;

Outer make();

void demo() {
  §(01_auto)auto value = make();
}

}
