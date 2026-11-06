namespace {

struct Entry {
    const char* name;
    bool fast;
};

}  // namespace

using Flag = bool;

static Entry make(const char* name, Flag fast) {
    return {name, fast};
}

int count() {
    static Entry entries[] = {
#define FAST(CHECK) make(#CHECK, true),
#include "checks.h"
    };
    return sizeof(entries) / sizeof(entries[0]);
}

int main() {
    return count();
}
