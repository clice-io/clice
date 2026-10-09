/// # Variadic template pack
///
/// - status: supported
///
/// The parameter pack renders as the callee's uninstantiated signature

template <typename... Args>
void emit(Args... args);

int main() {
    emit(§(pos));
}
