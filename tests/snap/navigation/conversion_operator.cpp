/// - verify: server

struct Number {
    int value;
    §(conv_decl)operator int() const;
};

Number::§(conv_def)operator int() const {
    return value;
}

int read_number(Number number) {
    return number.§(conv_use)operator int();
}
