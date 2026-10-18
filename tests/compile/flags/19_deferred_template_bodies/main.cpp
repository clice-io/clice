struct Gauge {
    int level;
};

template <class T>
int scaled(T value) {
    return value * factor;
}

int factor = 2;

template <class T>
int shifted(T value, Gauge gauge) {
    return value + gauge.§(deferred)level;
}

int doubled() {
    return §(call)scaled(21);
}
