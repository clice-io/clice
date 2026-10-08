/// # Clickable type names
///
/// - status: supported
/// - issues: clangd#1535
///
/// Each type name in a type hint links to its declaration: clicking it goes
/// to the definition, hovering it shows the type's card
///
/// A class declared ahead of its definition links to that declaration, from
/// which go-to-definition reaches the definition. Template arguments link one
/// by one; builtin types and punctuation stay plain text.

#include "widget.h"

template <typename T, typename U>
struct Pair {};

Pair<Widget, Gadget> make_pair();
const Widget* find(int id);

void use() {
    auto pair = make_pair();
    auto found = find(1);
}
