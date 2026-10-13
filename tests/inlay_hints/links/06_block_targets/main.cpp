// The name after a closing brace or an `#endif` links to where its block
// starts: the function, the statement, the namespace, the `#if`.
namespace storage {

#ifdef ENABLE_CACHE
int cached();
#endif

int count(int total) {
    while (total > 0) {
        total -= 1;
    }
    return total;
}

}
