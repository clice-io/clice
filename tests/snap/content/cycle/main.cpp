// - verify: inspect

// Mutually dependent units: the recursion pair and the two structs
// pointing at each other form cycles in the dependency graph.
#include "mutual.h"

int start() { return ping(3); }
