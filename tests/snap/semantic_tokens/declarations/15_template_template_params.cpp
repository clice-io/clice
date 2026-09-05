/// # Template template parameters
///
/// - status: supported
///
/// Template-template parameters receive type tokens at declaration and use

template <typename T>
struct Holder {};

template <template <typename> class §Container, typename T>
struct Adaptor {
    §Container<T> value;
};

Adaptor<Holder, int> adaptor;
