// Pins the notes of a finding: they follow it, into a header too. The
// header's declaration of function() mismatches the definition here and the
// one in b.cpp: the same warning on the same line from two units, whose notes
// name different definitions, stays two findings.
#include "common.h"

void function(int alpha) {
    (void)alpha;
}

void run() {
    call(/*disabled=*/true);
}
