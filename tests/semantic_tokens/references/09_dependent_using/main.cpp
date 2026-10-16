// A dependent using declaration's name is unknown until an instantiation.
template <typename T>
struct Derived : T {
    using T::§(value_use)value;

    int use() {
        return §(value_use2)value;
    }
};
