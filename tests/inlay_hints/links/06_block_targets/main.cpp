// The name after a closing brace or an `#endif` links to where its block
// starts: the function or type name, the statement, the namespace, the
// `#if`.
namespace storage {

#ifdef ENABLE_CACHE
int cached();
#endif

struct Counter {
    int total;
};

int count(int total) {
    while (total > 0) {
        total -= 1;
    }
    if (total < 0) {
        total = 0;
    }
    return total;
}

}
