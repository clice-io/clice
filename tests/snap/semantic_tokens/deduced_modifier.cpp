/// # Token Modifiers
///
/// ## Deduced — mark deduced types such as `auto` and `decltype`
///
/// - status: unsupported
/// - order: 9

auto deduced_int = 1;
decltype(deduced_int) same_type = 2;
