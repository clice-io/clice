// Dependent using declarations remain unpainted.
template <typename T>
struct Derived : T {
    using T::§(value_use)value;

    int use() {
        return §(value_use2)value;
    }
};
