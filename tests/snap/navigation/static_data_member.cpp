/// - verify: server

struct Counter {
    static int §(member_decl)value;
};

int Counter::§(member_def)value = 1;

int read_counter() {
    return Counter::§(member_use)value;
}
