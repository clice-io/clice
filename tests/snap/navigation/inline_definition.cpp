/// - verify: server

int §(def)twice(int value) {
    return value + value;
}

int caller() {
    return §(use)twice(21);
}
