// The references of a function template are all its calls, whatever
// arguments it is specialized with, deduced or written.

template <typename T>
T §(declared)twice(T value) {
    return value + value;
}

int ints() {
    return twice(1) + twice<int>(2);
}

double doubles() {
    return §(call)twice(1.5);
}
