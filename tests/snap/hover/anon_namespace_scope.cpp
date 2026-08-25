/// # Symbol Information
///
/// ## Anonymous namespace scope — `(anonymous namespace)` shows in the scope display
///
/// - status: partial
/// - order: 7
/// - issues: clangd#436
///
/// The cards render, but the anonymous segment is dropped from the
/// scope display: a top-level anonymous member shows no scope line at
/// all, and `outer::(anonymous)` shows just `outer`.

namespace {
int hid§(anon_var)den = 1;
}

namespace outer {
namespace {
int nes§(nested_anon_var)ted = 2;
}
}

int sum = hidden + outer::nested;
