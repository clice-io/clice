// Template-template parameters receive type tokens at declaration and use.
template <typename T>
struct Holder {};

template <template <typename> class §(Container_def)Container, typename T>
struct Adaptor {
    §(Container_use)Container<T> value;
};

Adaptor<Holder, int> adaptor;
