/// # Hover Correctness
///
/// ## MSVC inheritance model — `MSInheritanceAttr` does not corrupt record hover
///
/// - status: supported
/// - order: 1
/// - flags: ["--target=x86_64-pc-windows-msvc"]
///
/// clangd tracks this as clangd#1643 and clangd#2212; under an MSVC target
/// the implicit inheritance attribute does not leak into the record or
/// method card.

namespace ms {

struct Wid§(struct_hover)get {
    int value;
    void up§(method_hover)date();
};

int Widget::* member = &Widget::value;

}
