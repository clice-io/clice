// The members of an anonymous union at global scope are fields, declared
// in the union and used by name, as inside a namespace.

static union {
    int u1;
    float u2;
};

int read() {
    return u1;
}
