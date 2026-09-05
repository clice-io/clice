/// # MSVC inheritance model
///
/// - status: supported
/// - flags: ["--target=x86_64-pc-windows-msvc"]
///
/// MSVC inheritance model attributes do not alter record hover
///
/// Under an MSVC target, the implicit inheritance attribute does not leak
/// into the record or method card.

namespace ms {

struct Wid§(struct_hover)get {
    int value;
    void up§(method_hover)date();
};

int Widget::* member = &Widget::value;

}
