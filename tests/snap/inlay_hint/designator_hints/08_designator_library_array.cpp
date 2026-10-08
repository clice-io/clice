/// # Library arrays
///
/// - status: supported
///
/// The lone member array of a `std::array`-like wrapper stays out of the designator

template <typename T, unsigned N>
struct Array {
    T _Elems[N];
};

Array<int, 2> pair{1, 2};

// Elided braces index through both wrappers at once.
Array<Array<int, 2>, 2> grid{1, 2, 3, 4};
