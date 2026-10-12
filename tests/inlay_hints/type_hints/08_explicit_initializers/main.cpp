int compute();

void use() {
    §(spelled)⟦auto widened = static_cast<long>(compute());
    auto braced = int{42};⟧
    §(deduced)⟦auto plain = compute();⟧
}
