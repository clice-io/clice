/// # Destructors of class templates — the `~` shape holds under templates
///
/// - status: supported

template <typename T>
struct Holder {
    §~§Holder();
};

template <typename T>
Holder<T>::§~§Holder() {}
