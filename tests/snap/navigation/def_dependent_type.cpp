/// # Go to Definition
///
/// ## Dependent member navigation in uninstantiated templates
///
/// - status: supported
/// - verify: server
/// - order: 10
///
/// Inside a template that is never instantiated, a member accessed on an
/// object of a dependent type resolves to the member declared on the
/// corresponding class template.

template <typename T>
struct Sink {
    void §(member_decl)push(T value);
};

template <typename T>
void drain(Sink<T>& sink, T value) {
    sink.§(member_call)push(value);
}
