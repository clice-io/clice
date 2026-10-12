void scale(int value);

void scale(double value) {}

template <typename T>
void apply(T value) {
    §(call)scale(value);
}
