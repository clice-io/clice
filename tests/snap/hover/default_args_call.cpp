/// # Hover Correctness
///
/// ## Call with default arguments — hovering a call that omits defaults does not crash
///
/// - status: supported
/// - order: 4
///
/// clangd crashes on this (clangd#551); clice renders the callee signature
/// with its default arguments.

namespace defaults {

int compute(int a, int b = 10, int c = 20);

int result = comp§(call_site)ute(1);

}
