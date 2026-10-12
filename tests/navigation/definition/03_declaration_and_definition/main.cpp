int §(decl)scale(int value);

int §(def)scale(int value) {
    return value * 2;
}

int apply(int value) {
    return §(use)scale(value);
}

int §(inline_def)twice(int value) {
    return value + value;
}

int caller() {
    return §(inline_use)twice(21);
}
