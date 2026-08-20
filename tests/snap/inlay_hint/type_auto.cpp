/// # Type Hints
///
/// ## Deduced `auto` variables — the hint shows the full variable type, qualifiers included
///
/// - status: supported
/// - order: 1

int make();

void use() {
    auto value = make();
    const auto& ref = value;
    auto* ptr = &value;
}
