/// - verify: server
///
/// Tokens inside a `#define` body have no meaning of their own — each
/// expansion assigns one — so nothing anchors there and navigation on a
/// body-spelled name stays `none`. The generated symbols anchor at the
/// invocation instead: uses spelled in real code navigate to the
/// invocation row, and the invocation token itself belongs to the macro.

#define DEFINE_COUNTER int §(body_name)counter_value = 0; int next_value()

#define MAKE_FLAG(name) bool flag_##name = false

§(counter_site)DEFINE_COUNTER {
    return §(counter_use)counter_value + 1;
}

§(flag_site)MAKE_FLAG(verbose);

bool read_flag() {
    return §(flag_use)flag_verbose;
}
