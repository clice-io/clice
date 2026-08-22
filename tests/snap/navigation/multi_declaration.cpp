/// - verify: server

int §(first_decl)clamp(int value);
int §(second_decl)clamp(int value);

int §(def)clamp(int value) {
    return value < 0 ? 0 : value;
}

int hold(int value) {
    return §(use)clamp(value);
}
