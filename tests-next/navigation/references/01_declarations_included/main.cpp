int §(decl)scale(int value);

int §(def)scale(int value) {
    return value * 2;
}

int use() {
    return §(use)scale(3);
}
