/// # Overload Signatures
///
/// ## C-style variadic function — named parameters are listed while the trailing ellipsis is elided from the label
///
/// - status: supported
/// - order: 12

void record(int code, ...);

int main() {
    record(§(pos)0);
}
