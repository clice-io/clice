#include "lib.h"

namespace app {

int run() {
    return helper§(using_declaration);
}

}  // namespace app

using namespace lib;

int use() {
    return helper§(using_directive);
}

int qualified() {
    return lib::helper§(namespace_member);
}

int aliased() {
    return inner::§(namespace_alias);
}
