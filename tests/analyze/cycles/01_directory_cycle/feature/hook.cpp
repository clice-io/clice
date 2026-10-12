#include "feature/hook.h"
#include "core/log.h"

int hook(Logger* logger) {
    return logger ? logger->clock.size() : 0;
}
