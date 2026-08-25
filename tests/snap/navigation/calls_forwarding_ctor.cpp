/// # Call Hierarchy
///
/// ## Constructor calls through forwarding functions
///
/// - status: unsupported
/// - order: 9
/// - issues: clangd#2242
///
/// Incoming calls of a constructor do not include the call sites that
/// reach it through a perfect-forwarding factory.

template <typename T, typename... Args>
T make(Args&&... args) {
    return T(static_cast<Args&&>(args)...);
}

struct Widget {
    Widget(int w, int h);  // make<Widget> below is absent from incoming calls
};

Widget build() {
    return make<Widget>(800, 600);
}
