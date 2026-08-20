/// # Overload Signatures
///
/// ## Default arguments in the label — parameters with defaults render their initializer in the signature
///
/// - status: supported
/// - order: 11

void configure(int width, int height = 100, bool visible = true);

int main() {
    configure(§(pos)1);
}
