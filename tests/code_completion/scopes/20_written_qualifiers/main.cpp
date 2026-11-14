// A name the file does not include yet completes after the scope written
// before it, through the header declaring it.
#include "stream.h"

namespace alias = store;

#define STORE store

struct Local {
    static int vessel_count;
};

void use() {
    store::vec§(qualified);
    ::store::vec§(global);
    store::inner::Ves§(nested);
    alias::vec§(namespace_alias);
    STORE::vec§(macro_qualifier);
    outer::Gad§(undeclared_namespace);
    Local::ves§(class_scope);
}
