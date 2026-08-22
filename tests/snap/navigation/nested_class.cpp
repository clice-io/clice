/// - verify: server

struct Outer {
    struct §(inner_def)Inner {
        int §(member_decl)read() const;
    };
};

int Outer::§(inner_scope)Inner::§(member_def)read() const {
    return 1;
}

int read_inner(const Outer::Inner& inner) {
    return inner.§(member_use)read();
}
