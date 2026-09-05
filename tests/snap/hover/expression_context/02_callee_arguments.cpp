/// # Call arguments
///
/// - status: supported
///
/// Which parameter each argument binds to
///
/// Hovering an argument at a call site shows the parameter it is passed to,
/// naming the parameter it binds.

namespace parameter_binding {
void resize(long height, long& previous, long flags = 0);

void apply() {
    long height = 720;
    long previous = 0;
    resize(§(01_by_name)height, §(02_by_ref)previous, §(03_literal)2);
}
}
