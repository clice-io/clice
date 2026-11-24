#include "pkg/internal.h"
#include "pkg/lib.h"
#include "other/dep.h"

int main() {
    return internal() + lib() + dep();
}
