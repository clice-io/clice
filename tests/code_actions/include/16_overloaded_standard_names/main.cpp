// Standard names the mapping lists under several headers: std::move, the
// most common include a file misses, and std::size_t, which many C headers
// declare.

#include "config.h"

struct Box {};

void f(Box box) {
    Box other = std::§(move)move(box);
    std::§(size_t)size_t count = 0;
    Box kept = std::move§(commented)/* the box */ (/* again */ other);
    Box last = std::/* the name */ §(qualified)move(kept);
}
