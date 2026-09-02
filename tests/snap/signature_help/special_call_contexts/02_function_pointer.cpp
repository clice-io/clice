/// # Function pointer calls — the prototype's parameter names show, not just the types
///
/// - status: supported

int main() {
    void (*callback)(int code, double value) = nullptr;
    callback(§(pos)5, 1.5);
}
