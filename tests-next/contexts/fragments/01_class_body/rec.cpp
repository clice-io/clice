#include "base.h"

struct Record : Base {
#include "body.inc"
};

int main() {
    Record r;
    return r.sum();
}
