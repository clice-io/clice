struct Handle {
    Handle(int value) : value(value) {}

    Handle(Handle&&) = default;
    Handle& operator=(Handle&&) = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    int value;
};

struct §(reference)Reference {
    Reference(int& ref) : ref(ref) {}

    int& ref;
};

struct §(constant)Constant {
    Constant(int limit) : limit(limit) {}

    const int limit;
};

struct §(holder)Holder {
    Holder(int value) : handle(value) {}

    Handle handle;
};
