#define X(name) int name = 1;
#include "list.def"
#undef X
#define X(name) void get_##name();
#include "list.def"
#undef X
int main() { return alpha; }
