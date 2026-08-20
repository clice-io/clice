/// # Parameter Hints
///
/// ## Forwarding resolution — packs forwarded through wrappers resolve to the target's parameter names
///
/// - status: supported
/// - issues: clangd#2324
/// - order: 5

namespace std {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

}  // namespace std

void target(int first, int second);

template <typename... Args>
void wrap(Args&&... args) {
    target(std::forward<Args>(args)...);
}

// A plain pass-through works without std::forward as well.
void sink(int a, int b, int c);

template <typename... Ts>
void call_with(Ts... ts) {
    sink(ts...);
}

// Forwarding also resolves through packs sandwiched between fixed
// head and tail arguments.
int accumulate(int, int b, double);

template <typename... Args>
int head_tail(int a, Args&&... args) {
    return accumulate(1, std::forward<Args>(args)..., 1.0);
}

template <typename... Args>
int chain(Args&&... args) {
    return head_tail(std::forward<Args>(args)...);
}

void use() {
    wrap(1, 2);
    call_with(1, 2, 3);
    chain(32, 42);
}
