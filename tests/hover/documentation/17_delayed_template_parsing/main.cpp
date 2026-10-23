template <typename T>
struct Gauge {
    T level;
    T getLevel() { return level; }
    void setLevel(T value) { level = value; }
};

template <typename T>
T read(Gauge<T>& gauge) {
    return gauge.§(dependent_getter)getLevel();
}

template <typename T>
void write(Gauge<T>& gauge, T value) {
    gauge.§(dependent_setter)setLevel(value);
}
