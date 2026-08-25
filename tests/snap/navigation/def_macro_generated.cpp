/// # Go to Definition
///
/// ## Names conjured by a macro body or token paste anchor at the invocation
///
/// - status: supported
/// - verify: server
/// - order: 7
///
/// A name assembled by token paste has no spelling of its own in the
/// source, so it anchors at the macro invocation that creates it: the
/// invocation is its definition site, and a plain use of the name jumps
/// back to that invocation.

#define MAKE_FLAG(name) bool flag_##name = false

§(flag_site)MAKE_FLAG(verbose);

bool read_flag() {
    return §(flag_use)flag_verbose;
}
