#include "header_a.h"
#include "header_b.h"
int x = 1;
#include "header_c.h"

const char data[] = {
#embed "data.bin"
};

#if __has_embed("data.bin")
int has_embed_found = 1;
#endif

#if __has_embed("no_such_file.bin")
int has_embed_not_found = 1;
#endif

int main() {
    return a + b + c;
}
