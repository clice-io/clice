#include §(preamble_include)"panel.h"

int build() {
    return dimension();
}

#include §(late_include)"extra.h"

int total() {
    return build() + spacing();
}
