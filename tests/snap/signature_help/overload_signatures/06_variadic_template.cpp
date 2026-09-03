/// # Variadic template pack — the parameter pack renders as the callee's uninstantiated signature
///
/// - status: supported

template <typename... Args>
void emit(Args... args);

int main() {
    emit(§(pos));
}
