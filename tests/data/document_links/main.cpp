#include "header_a.h"
#include "header_b.h"
int x = 1;
#include "header_c.h"

const char data[] = {
#embed "data.bin"
};

int main() {
    return a + b + c;
}
