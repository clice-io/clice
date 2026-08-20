/// # Symbol Kinds
///
/// ## Template declarations — class, function and variable templates carry a `template ` detail prefix; concepts and abbreviated function templates (`concept auto` parameters) appear as well
///
/// - status: supported
/// - order: 2

namespace templates {

template <typename T>
struct Box {
    T value;

    void reset();
};

template <typename T>
void Box<T>::reset() {}

template <typename T>
T zero() {
    return T();
}

template <typename T>
constexpr T pi = T(3.14159);

template <typename T>
concept Small = sizeof(T) <= 4;

void takes_concept(Small auto x);

}  // namespace templates
