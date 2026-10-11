// An unqualified name also tries the std namespace of the standard library
// mapping: printf offers <cstdio>.

#include "config.h"

void f() {
    §(printf)printf("%d", 1);
}
