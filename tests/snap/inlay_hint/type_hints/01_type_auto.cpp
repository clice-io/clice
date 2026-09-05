/// # Deduced `auto` variables
///
/// - status: supported
///
/// The hint shows the full variable type, qualifiers included

int make();

void use() {
    auto value = make();
    const auto& ref = value;
    auto* ptr = &value;
}
