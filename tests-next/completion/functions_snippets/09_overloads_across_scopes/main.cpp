// Same-named functions from different namespaces in scope bundle into one
// entry that counts all of them.
namespace a {
void draw(int x);
}

namespace b {
void draw(double x);
}

using namespace a;
using namespace b;

void bar() {
    dra§(pos);
}
