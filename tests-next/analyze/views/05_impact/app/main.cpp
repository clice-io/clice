#include "app/detail.h"
#include "util/clock.h"

int main() {
    Clock clock{};
    return base_size() + clock.name.n;
}
