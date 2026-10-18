#include "extra.h"

static_assert(level() == 2);

int other() {
    return §(other)level() + doubled();
}
