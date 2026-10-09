/// # No clear counterpart
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// When no candidate clearly outweighs the others, the editor lists them with their reasons to pick from

// switch: codec.h

#include "codec.h"

int round_trip(int value) {
    return decode(encode(value));
}
