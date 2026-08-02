/// # Overload Signatures
///
/// ## Variadic template pack — the parameter pack renders as the callee's uninstantiated signature
///
/// - status: supported
/// - order: 13

template <typename... Args>
void emit(Args... args);

int main() {
    emit(§(pos));
}
