/// # Include Directives
///
/// ## `#include_next` and `__has_include_next` — links continue down the search path
///
/// - status: partial
/// - order: 4
/// - flags: ["-I${corpus}/include_next/first", "-I${corpus}/include_next/second"]
///
/// `first/wrap.h` shadows `second/wrap.h` on the search path; its
/// `#include_next` (guarded by `__has_include_next`) includes the second
/// copy. Next-in-path resolution only exists when the header is compiled
/// in an including TU's context — opened standalone it is compiled as its
/// own TU, where clang deliberately treats `#include_next` as a plain
/// include, so today both links land back on the first copy (as the
/// snapshot pins).

#include <wrap.h>

int use_wrap = WRAP_FIRST + WRAP_SECOND;
