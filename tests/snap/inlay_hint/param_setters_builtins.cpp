/// # Parameter Hints
///
/// ## Setter and builtin suppression — `setX(x)` and `std::move`/`std::forward` arguments stay bare
///
/// - status: supported
/// - order: 3

namespace std {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
struct remove_reference<T&> {
    using type = T;
};

template <typename T>
struct remove_reference<T&&> {
    using type = T;
};

template <typename T>
constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

template <typename T>
constexpr typename remove_reference<T>::type&& move(T&& t) noexcept;

}  // namespace std

struct Config {
    void setWidth(int width);
    // The parameter carries extra information beyond the setter name, so
    // it still hints.
    void setTimeout(int timeout_millis);
};

void consume(int&& sink);

void use(Config& config) {
    config.setWidth(3);
    config.setTimeout(5);
    int value = 1;
    consume(std::move(value));
}
