// feature/hook.inc is pasted into main.cpp: what it names of util/clock.h
// is app's use, not feature's.
#include "util/clock.h"
#include "feature/hook.inc"

int main() {
    return hooked();
}
