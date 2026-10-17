template <typename T>
bool less(T a, unsigned b) {
    return a < b;
}

template <typename T>
bool check(T a) {
    return less(a, 1u);
}

inline bool header_use() {
    return check(-1);
}
