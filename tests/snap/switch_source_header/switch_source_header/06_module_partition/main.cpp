/// # Partition beside its implementation
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// An internal partition declaring what an implementation unit of the same name defines pairs with that unit, ahead of the primary interface

// switch: api.cppm
// switch: api.cpp

module lib;

import :api;

int run() {
    return lookup(1);
}
