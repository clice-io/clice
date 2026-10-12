#include "core/key.h"
#include "core/traits.h"
#include "util/clock.h"

int main() {
    Key key{};
    return Traits<Str>::value + Traits<Key>::value + key.k + Hash<Clock>::seed;
}
