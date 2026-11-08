#define §(limit)LIMIT 10
#define OTHER 1

#ifdef FEATURE_X
#endif

namespace config {
int count = LIMIT + OTHER;
}

int main() { return config::count; }
