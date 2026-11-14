#pragma once

struct Stream {
    int read() {
        return 1;
    }

    template <class T>
    T get() {
        return T(read());
    }
};

inline int consume(Stream& stream) {
    return stream.read();
}
