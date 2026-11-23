// A member that copies only from a non-const object makes the implicit copy
// operations take their parameter by non-const reference; declared with a
// const one they would be deleted.

struct Legacy {
    Legacy() = default;
    Legacy(Legacy&) {}
    Legacy& operator=(Legacy&) {
        return *this;
    }
};

struct §(wrapper)Wrapper {
    Wrapper(int id) : id(id) {}

    Legacy legacy;
    int id;
};

void use(Wrapper& wrapper) {
    Wrapper copy = wrapper;
    copy = wrapper;
}
