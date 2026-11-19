struct §(copy_declared)CopyDeclared {
    CopyDeclared(const CopyDeclared&) = default;
    CopyDeclared& operator=(const CopyDeclared&) = default;

    int value;
};

struct §(move_declared)MoveDeclared {
    MoveDeclared(MoveDeclared&&) = default;
    MoveDeclared& operator=(MoveDeclared&&) = default;

    int value;
};
