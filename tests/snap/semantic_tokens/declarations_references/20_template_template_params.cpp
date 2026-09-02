/// # Template template parameters — declared and used as types
///
/// - status: supported

template <typename T>
struct Holder {};

template <template <typename> class §Container, typename T>
struct Adaptor {
    §Container<T> value;
};

Adaptor<Holder, int> adaptor;
