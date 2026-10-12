#include "core/key.h"
#include "core/traits.h"

int main() {
    return Traits<Str>::value + Traits<Key>::value + Traits<int>::value;
}
