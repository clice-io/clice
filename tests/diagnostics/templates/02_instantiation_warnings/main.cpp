// A warning from an instantiation shows on the request as an error does,
// saying it comes from the template and pointing at where. The template is
// in a header whose function bodies the editor skips: the background index
// reports it.
#include "cmp.h"
bool b = less(-1, 1u);
