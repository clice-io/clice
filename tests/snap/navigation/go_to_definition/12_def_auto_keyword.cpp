/// # Deduced `auto` type navigation
///
/// - status: supported
/// - verify: server
///
/// Go-to-definition on `auto` reaches the type it was deduced to, as if the
/// type were written in its place
///
/// Go-to-type-definition on the keyword reaches the same type, and
/// find-references from it lists the type's uses. The keyword itself is no
/// use of the type: find-references from the type does not list it.

struct §(type)Widget {};

Widget make_widget();

void use() {
    au§(auto_keyword)to widget = make_widget();
}
