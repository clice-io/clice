// The `~` shape holds under templates.
template <typename T>
struct Holder {
    §(tilde_decl)~§(Holder_use)Holder();
};

template <typename T>
Holder<T>::§(tilde_def)~§(Holder_use2)Holder() {}
