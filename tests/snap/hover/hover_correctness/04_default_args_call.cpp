/// # Call with default arguments
///
/// - status: supported
///
/// Hovering a call that omits defaults does not crash
///
/// clangd crashes on this (clangd#551); clice renders the callee signature
/// with its default arguments.

namespace defaults {

int compute(int a, int b = 10, int c = 20);

int result = comp§(call_site)ute(1);

}
