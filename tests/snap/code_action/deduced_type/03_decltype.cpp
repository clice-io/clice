/// # Expand decltype
///
/// - status: supported
///
/// A `decltype` specifier expands to the type it denotes

int value();

void f() {
    §(dt)decltype(value()) x = 0;
}
