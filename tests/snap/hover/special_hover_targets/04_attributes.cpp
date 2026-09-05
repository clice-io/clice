/// # Attribute documentation
///
/// - status: supported
/// - issues: clangd#1862
///
/// Hovering an attribute shows its description
///
/// The attribute's own documentation renders in the card, for both GNU
/// `__attribute__` spellings and C++ `[[...]]` attributes.

namespace attribute_cards {
void receive(char* __attribute__((non§(gnu_attribute)null, noescape)));

[[nodi§(std_attribute)scard]] long compute();
}
