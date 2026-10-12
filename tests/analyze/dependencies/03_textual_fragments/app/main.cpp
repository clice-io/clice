// A fragment's names are charged to the file pasting it in, and what it
// defines belongs to that file: feature names nothing of util, and app
// nothing of feature.
#include "util/clock.h"
#include "feature/hook.inc"

int main() {
    return hooked();
}
