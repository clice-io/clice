// Defines what core/api.h declares, in another module.
#include "core/api.h"
#include "core/log.h"
#include "feature/dup.h"
#include "feature/f.h"

int feature_hook(Logger* logger) {
    Dup dup{};
    return logger ? logger->clock.ticks : dup.x;
}

int api() {
    return 1;
}
