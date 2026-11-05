// A namespace's names the index files under another scope: one of C
// linkage, and one a using-declaration in its anonymous namespace brings in.
#include "lib.h"

int qualified() {
    return api::pick§(qualified);
}

namespace api {

int unqualified() {
    return pick§(unqualified);
}

}  // namespace api
