#include "base.h"

struct Record : Base {
#include "body.inc"
    int field_c;
};

int main() {
    Record r;
    return r.sum();
}
