#include "stream.h"

namespace alias = store;

#define NS store

struct Local {
    static int vessel_count;
};

void use() {
    store::vec§(qualified);
    ::store::vec§(global);
    store::inner::Ves§(nested);
    alias::vec§(namespace_alias);
    NS::vec§(macro_qualifier);
    outer::Gad§(undeclared_namespace);
    Local::ves§(class_scope);
    store::§(nothing_typed);
}

namespace outer {
void nest() {
    inner::Wid§(relative_qualifier);
}
}  // namespace outer
