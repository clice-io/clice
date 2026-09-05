namespace local_object {
int update() {
    int counter = 0;
    §(01_local_var)counter = 3;
    return counter;
}
}

namespace method_object {
struct Tracker {
    void update() {
        long position = 0;
        §(02_method_local_var)position = 8;
    }
};
}

static int requests = 4;
void bump_requests() {
    §(03_global_var)requests++;
}

namespace metrics {
static long long bytes = -36637162602497;
}
void bump_bytes() {
    metrics::§(04_ns_global_var)bytes++;
}

namespace service {
namespace {
int generation;
}
}
int next_generation() {
    return ++service::§(05_anon_ns_var)generation;
}
