template <typename T>
struct Counter {
    static int value;
};

inline int read() {
    return Counter<int>::value;
}
