/// # Type Information
///
/// ## Lambda `auto` parameters — deduced parameter type
///
/// - status: unsupported
/// - issues: clangd#493
/// - order: 9
///
/// Hovering the `auto` parameter of a generic lambda yields no card; the
/// deduced parameter type is not shown.

namespace lambda_auto_params {

auto printer = [](auto value) { return value; };

}
