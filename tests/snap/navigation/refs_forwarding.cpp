/// # Find References
///
/// ## References through forwarding functions
///
/// - status: unsupported
/// - order: 5
/// - issues: clangd#716, clangd#1872
///
/// Find references on a constructor does not include call sites that reach
/// it indirectly through a perfect-forwarding factory.

template <typename T, typename... Args>
T make(Args&&... args) {
    return T(static_cast<Args&&>(args)...);
}

struct Widget {
    Widget(int w, int h);  // find-refs here omits the make<Widget> call
};

Widget build() {
    return make<Widget>(800, 600);
}
