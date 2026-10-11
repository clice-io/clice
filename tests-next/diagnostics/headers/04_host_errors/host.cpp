struct S { int v; };
int before_include = "x";
#define FROM_HOST 1
#include "frag.h"
int after_include = undeclared_after;
int main() { return 0; }
