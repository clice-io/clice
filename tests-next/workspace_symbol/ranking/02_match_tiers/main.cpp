// The exact name ranks first, then names starting with the query, then
// names merely containing it, whatever order the index holds them in: the
// server cuts the reply at its limit after ranking, so a weak match never
// pushes the exact one out.

int xfoo() {
    return 3;
}

int foobar() {
    return 2;
}

int foo() {
    return 1;
}
