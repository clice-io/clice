/// # Function signatures
///
/// - status: supported
/// - issues: clangd#520, clangd#601, clangd#1232
///
/// Parameter and return types in the `detail` field disambiguate overloads;
/// constructors drop the `void` return type

namespace detail {

void process(int x);
void process(const char* s);

struct Task {
    Task();
    Task(int priority);

    int run(bool async) const;
};

}  // namespace detail
