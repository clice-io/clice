#include "util.h"
#include "wrap.h"
#include "relay.h"
int main() { lib::vec<int> v; return remove_bad(v, 0) + remove_worse(v, 0) + reset(v); }
