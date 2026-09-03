/// # Deduced `auto` variables — the hint shows the full variable type, qualifiers included
///
/// - status: supported

int make();

void use() {
    auto value = make();
    const auto& ref = value;
    auto* ptr = &value;
}
