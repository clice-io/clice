/// # Expand decltype
///
/// - status: supported
///
/// A `decltype` specifier expands to the type it denotes, through any
/// `decltype` that type was itself declared with

int value();

void f() {
    §(dt)decltype(value()) x = 0;
    §(nested)decltype(x) y = 0;
}
