/// # Default arguments in the label
///
/// - status: supported
///
/// Parameters with defaults render their initializer in the signature

void configure(int width, int height = 100, bool visible = true);

int main() {
    configure(§(pos)1);
}
