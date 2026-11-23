struct §(move_declared)MoveDeclared {
    MoveDeclared(MoveDeclared&&) = default;
    MoveDeclared& operator=(MoveDeclared&&) = default;

    int value;
};

struct §(copy_declared)CopyDeclared {
    CopyDeclared(int value) : value(value) {}
    CopyDeclared(const CopyDeclared& other) : value(other.value) {}

    int value;
};

void use(CopyDeclared& declared) {
    CopyDeclared copy = static_cast<CopyDeclared&&>(declared);
    copy = static_cast<CopyDeclared&&>(declared);
}
