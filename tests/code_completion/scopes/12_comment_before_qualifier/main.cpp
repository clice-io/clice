// The line above a qualifier ends a sentence: its period is no member
// access.
#include "lib.h"

void use() {
    // The months of the year.
    lib::§(after_comment);
}
