#include "core/config.h"
#include "core/fast.h"
#include "core/level.h"
#include "core/mode.h"
#include "core/width.h"
#include "third/lib.h"

int main() {
    ModeA mode;
    Third third{};
    return fast() + level() + sizeof(mode) + third.v + width();
}
