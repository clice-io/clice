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
    void set_height(int height);
    // The parameter carries extra information beyond the setter name, so
    // it still hints.
    void setTimeout(int timeout_millis);
};

void consume(int&& sink);

// The three-argument algorithm form of std::move is a real call whose
// parameters deserve hints; only the single-argument cast stays bare.
namespace std {

template <typename T>
T* move(T* first, T* last, T* result);

}  // namespace std

void use(Config& config) {
    config.setWidth(3);
    config.set_height(4);
    config.setTimeout(5);
    int value = 1;
    consume(std::move(value));
    int buffer[4];
    std::move(buffer, buffer + 2, buffer + 2);
}
