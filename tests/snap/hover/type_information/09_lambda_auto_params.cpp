/// # Lambda `auto` parameters
///
/// - status: unsupported
/// - issues: clangd#493
///
/// Generic lambda parameters do not show their deduced type yet
///
/// Hovering the `auto` parameter of a generic lambda yields no card; the
/// deduced parameter type is not shown.

namespace lambda_auto_params {

auto printer = [](auto value) { return value; };

}
