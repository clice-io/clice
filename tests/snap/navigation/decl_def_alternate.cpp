/// - verify: server

int §(decl)scale(int value);

int §(def)scale(int value) {
    return value * 2;
}

int apply(int value) {
    return §(use)scale(value);
}
