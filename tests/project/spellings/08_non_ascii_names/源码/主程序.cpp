#include §(include)"工具.h"

int main() {
    const char* text = §(macro)GREETING;
    return §(call)helper() + (text[0] == 0);
}
