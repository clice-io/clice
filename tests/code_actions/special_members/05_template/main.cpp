// A class template's members are only known once it is instantiated, so none
// of the four operations can be evaluated on the pattern: all come out
// deleted.

template <typename T>
struct §(box)Box {
    Box(T value) : value(value) {}

    T value;
};
