#pragma once

[[deprecated("use fresh")]] inline int stale() {
    return 0;
}

void take(int first, int second);

struct Closed {
private:
    int secret = 0;
};

template <class T>
int needs_size(T value) {
    return value.size();
}
