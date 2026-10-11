#include "core/log.h"
#include "meta/m.h"

int main() {
    Logger logger{};
    return logger.clock.size() + meta_size();
}
