/// # Partition beside its implementation
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// An internal partition and the implementation unit of the same name defining its declarations switch to each other directly
///
/// The implementation unit also lists the primary interface of its module,
/// after the partition.

// switch: api.cppm
// switch: api.cpp

module lib;

import :api;

int run() {
    return lookup(1);
}
