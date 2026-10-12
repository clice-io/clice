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

template <typename T>
struct Store {
    T §(dependent_field)stored;
};

template <typename T>
struct Reader : Store<T> {
    T read() {
        return this->stored;
    }
};

int next() {
    return counter++;
}

Mode pick() {
    return Fast;
}
