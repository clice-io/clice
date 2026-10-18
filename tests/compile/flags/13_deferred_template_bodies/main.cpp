template <class T>
int scaled(T value) {
    return value * factor;
}

int factor = 2;

template <class T>
int shifted(T value) {
    return value + §(deferred)factor;
}

int doubled() {
    return §(call)scaled(21);
}
