#include "util/clock.h"

int clock_size(const Clock& clock) {
    return length(clock.name);
}
