/// # Module name hover
///
/// - status: supported
/// - verify: server
///
/// Hovering a module name shows the interface unit that defines it
///
/// The card names the module and the file of its interface unit, on the
/// name in an `import` and in the module declaration alike.

// snap: Server only: the card comes from the index, which knows the defining
// snap: unit across files; the inspect path has no index to ask.

import §(import_name)math;

int twice() {
    return double_it(1);
}
