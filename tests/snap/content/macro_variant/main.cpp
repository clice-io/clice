// - verify: inspect

// A header whose expansion depends on the includer: the unit under the
// condition owns the disabled lines as text; the condition itself leaves
// no token, so only the header's own macro reaches the unit expanding it.
#define FAST 1
#include "config.h"

int run() { return configured() + plain(); }
