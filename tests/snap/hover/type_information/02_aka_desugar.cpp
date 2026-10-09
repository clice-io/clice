/// # Type aliases
///
/// - status: supported
/// - config: {"show_aka": false}
///
/// Hover can show the desugared `aka` form
///
/// A sugared type shows its underlying type as `Alias (aka int)`. The
/// `show_aka` option turns the `aka` suffix off.

namespace aka_desugar {

using Handle = int;
using Alias = Handle;

Handle §(01_alias)direct = 0;

Alias §(02_alias_chain)chained = 0;

}
