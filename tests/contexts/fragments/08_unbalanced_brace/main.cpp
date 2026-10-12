#define X(name) int name;
enum Ids {
#include "list.def"
};
#undef X
int main() { return 0; }
