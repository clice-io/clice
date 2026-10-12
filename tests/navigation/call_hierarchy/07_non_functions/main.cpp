int §(variable)counter = 0;

enum Mode {
    §(enumerator)Fast,
    Slow,
};

struct Tally {
    int §(field)hits = 0;

    void bump() {
        hits += 1;
    }
};

int next() {
    return counter++;
}

Mode pick() {
    return Fast;
}
