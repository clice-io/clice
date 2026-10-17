struct S {
#include "m.inc"
void g() { f(); }
};
int main() { S{}.g(); }
