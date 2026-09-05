/// # Lambda `auto` parameters
///
/// - status: unsupported
/// - issues: clangd#493
///
/// Deduced parameter type
///
/// Hovering the `auto` parameter of a generic lambda yields no card; the
/// deduced parameter type is not shown.

namespace lambda_auto_params {

auto printer = [](auto value) { return value; };

}
