int make();

void use() {
    auto value = make();
    const auto& ref = value;
    auto* ptr = &value;
}
