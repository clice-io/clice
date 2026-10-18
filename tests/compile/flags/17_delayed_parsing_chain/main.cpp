#include "extra.h"

struct Other {
    int value;
};

int run() {
    return top_template(Other{2}) + §(base)base_template(Other{3}) + use_base();
}
