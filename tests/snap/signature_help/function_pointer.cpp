/// # Special Call Contexts
///
/// ## Function pointer calls — the prototype's parameter names show, not just the types
///
/// - status: supported
/// - order: 2

int main() {
    void (*callback)(int code, double value) = nullptr;
    callback(§(pos)5, 1.5);
}
