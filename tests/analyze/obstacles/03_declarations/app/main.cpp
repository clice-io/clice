#include "core/api.h"
#include "feature/f.h"
#include "util/dup.h"

int main() {
    Dup dup{};
    return feature_hook(nullptr) + api() + dup.x;
}
