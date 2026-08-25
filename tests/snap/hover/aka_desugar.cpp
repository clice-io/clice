/// # Type Information
///
/// ## Type aliases — the desugared `aka` form
///
/// - status: supported
/// - order: 2
/// - config: {"show_aka": false}
///
/// A sugared type shows its underlying type as `Alias (aka int)`. The
/// `show_aka` option turns the `aka` suffix off.

namespace aka_desugar {

using Handle = int;
using Alias = Handle;

Handle §(01_alias)direct = 0;

Alias §(02_alias_chain)chained = 0;

}
