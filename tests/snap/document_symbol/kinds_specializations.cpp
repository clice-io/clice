/// # Symbol Kinds
///
/// ## Template specializations and deduction guides — explicit and partial specializations of class and variable templates appear with their template arguments in the name; members nest under their specialization; deduction guides render their deduced signature
///
/// - status: supported
/// - order: 3

namespace spec {

template <typename T>
struct Box {
    T value;
};

template <>
struct Box<void> {};

template <typename T>
struct Box<T*> {
    T* pointee;
};

template <typename T>
T zero() {
    return T();
}

template <>
int zero<int>();

template <typename T>
constexpr T pi = T(3);

template <>
constexpr int pi<int> = 3;

template <typename T>
constexpr T* pi<T*> = nullptr;

template <typename T>
struct Deduced {
    Deduced(T raw);
};

template <typename T>
Deduced(T*) -> Deduced<T>;

// Forces the implicit instantiation Box<int>, which must not appear.
Box<int> instantiated;

// An explicit class instantiation gets a childless node; the instantiated
// members and the function instantiation (whose location clang records at
// the primary) produce no symbols.
template struct Box<char>;
template long zero<long>();

}  // namespace spec
