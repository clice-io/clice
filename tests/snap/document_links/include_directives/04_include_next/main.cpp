/// # `#include_next` and `__has_include_next`
///
/// - status: partial
/// - flags: ["-I${corpus}/include_directives/04_include_next/first", "-I${corpus}/include_directives/04_include_next/second"]
///
/// Standalone `#include_next` links back to the first matching header
///
/// `first/wrap.h` shadows `second/wrap.h` on the search path; its
/// `#include_next` (guarded by `__has_include_next`) includes the second
/// copy. Next-in-path resolution only exists when the header is compiled
/// in an including TU's context — opened standalone it is compiled as its
/// own TU, where `#include_next` behaves as a plain include and both links
/// land back on the first copy.

#include <wrap.h>

int use_wrap = WRAP_FIRST + WRAP_SECOND;
