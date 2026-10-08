/// # clang-tidy fix
///
/// - status: supported
///
/// A clang-tidy finding's fix is offered as a quick fix
///
/// Without a `.clang-tidy` above the file, clangd's default checks run.

namespace detail {
int value;
}

using detail::§(using)value;
