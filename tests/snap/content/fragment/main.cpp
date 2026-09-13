// - verify: inspect

// A function body pulled in by an #include inside the braces is a
// fragment of the function's unit, not units of its own.
#include "compute.h"

int call() { return compute(2); }
