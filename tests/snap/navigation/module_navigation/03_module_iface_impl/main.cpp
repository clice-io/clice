/// # Module interface implementation navigation
///
/// - status: partial
/// - verify: server
///
/// Go-to-definition on the module name in an implementation unit
/// (`module m;`) jumps to the interface unit that declares the module;
/// the reverse direction, from the interface name to the implementation,
/// is not offered

import store;

int lookup(int key) {
    return fetch(key);
}
