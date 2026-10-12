inline void handle(int) {}
enum Ids { alpha, beta };
void register_all() {
#define X(name) handle(name);
#include "handlers.def"
#undef X
}
int main() { register_all(); return 0; }
