#include "core/fast.h"
#include "core/log.h"
#include "core/mode.h"
#include "util/clock.h"

int main() {
    Logger logger{};
    Mode mode{};
    Clock clock{};
    return fast() + mode.m + length(clock.name) + (logger.sink ? 1 : 0);
}
