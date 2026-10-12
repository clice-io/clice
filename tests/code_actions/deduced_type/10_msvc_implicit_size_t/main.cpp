// MSVC compatibility declares `::size_t` implicitly, without a source
// location: the type of `sizeof` expands to it.

void f() {
    §(size)auto size = sizeof(int);
}
