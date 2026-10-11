#pragma clang attribute push(__attribute__((annotate("legacy"))), apply_to = function)
#include "h.h"

void old_api();
#pragma clang attribute pop
