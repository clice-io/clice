template <typename T>
struct Sink {
    void §(member_decl)push(T value);
};

template <typename T>
void drain(Sink<T>& sink, T value) {
    sink.§(member_call)push(value);
}
