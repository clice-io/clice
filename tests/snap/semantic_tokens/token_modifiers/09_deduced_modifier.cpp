/// # Deduced
///
/// - status: unsupported
///
/// Mark deduced types such as `auto` and `decltype`

auto deduced_int = 1;
decltype(deduced_int) same_type = 2;
