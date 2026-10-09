/// # Destructors of class templates
///
/// - status: supported
///
/// The `~` shape holds under templates

template <typename T>
struct Holder {
    §~§Holder();
};

template <typename T>
Holder<T>::§~§Holder() {}
