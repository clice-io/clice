#ifdef CLICE_NEVER
struct Other {};
#include "utils.h"
#else
struct Posix {};
#include "utils.h"
#endif
int main() { make(); }
