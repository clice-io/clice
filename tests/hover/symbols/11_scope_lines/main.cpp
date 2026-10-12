// The scope line of a card names the namespaces, classes and functions
// around the symbol, for declarations and uses alike.

namespace local_object {
int update() {
    int counter = 0;
    §(local_var)counter = 3;
    return counter;
}
}

namespace method_object {
struct Tracker {
    void update() {
        long position = 0;
        §(method_local_var)position = 8;
    }
};
}

static int requests = 4;
void bump_requests() {
    §(global_var)requests++;
}

namespace metrics {
static long long bytes = -36637162602497;
}
void bump_bytes() {
    metrics::§(ns_global_var)bytes++;
}

namespace service {
namespace {
int generation;
}
}
int next_generation() {
    return ++service::§(anon_ns_var)generation;
}

/// Describes a free function.
void ren§(global_func)der() {}

namespace studio::detail {
/// Describes a namespaced function.
void flu§(ns_func)sh() {}
}

namespace studio::detail {
struct Painter {
    void paint() {
        int bru§(method_local)shes = 0;
    }
};
}

namespace studio {
namespace {
struct {
    char mo§(anon_struct_field)de;
} state;
}
}
