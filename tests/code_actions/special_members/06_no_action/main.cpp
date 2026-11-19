// The action has no body to append to on a forward declaration, nothing to
// declare on a union, and nothing to add to a class that already declares all
// four operations itself.

struct §(forward)Forward;

union §(value)Value {
    int integer;
    float real;
};

struct §(declared)Declared {
    Declared(const Declared&) = default;
    Declared(Declared&&) = default;
    Declared& operator=(const Declared&) = default;
    Declared& operator=(Declared&&) = default;
};
