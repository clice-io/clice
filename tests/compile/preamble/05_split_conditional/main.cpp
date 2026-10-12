// The leading directives end inside the #if: what is precompiled opens a
// branch the body closes.
#include "h.h"
#ifndef MISSING
#include "g.h"
int in_branch = helper() + §(other)other();
#else
#include "missing.h"
#endif

int main() {
    return in_branch;
}
