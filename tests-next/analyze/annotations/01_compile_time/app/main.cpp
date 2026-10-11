#include "core/fast.h"
#include "util/clock.h"

int main() {
    Clock clock{};
    return fast() + clock.name.n;
}
