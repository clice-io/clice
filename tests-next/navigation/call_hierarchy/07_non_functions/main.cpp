int §(variable)counter = 0;

enum Mode {
    §(enumerator)Fast,
    Slow,
};

int next() {
    return counter++;
}

Mode pick() {
    return Fast;
}
