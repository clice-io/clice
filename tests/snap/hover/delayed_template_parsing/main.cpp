// - snap: separate
// - flags: ["-fdelayed-template-parsing"]

// snap: The header lands in the preamble, which keeps clang's deferred
// snap: template bodies; the pattern is never instantiated here, so the
// snap: server sees no body and synthesizes no accessor documentation.
// snap: Inspect parses the whole unit eagerly and does.
#include "gauge.h"

template <typename T>
T read(Gauge<T>& gauge) {
    return gauge.§(dependent_getter)getLevel();
}
