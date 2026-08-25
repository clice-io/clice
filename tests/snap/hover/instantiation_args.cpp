/// # Type Information
///
/// ## Instantiation arguments — template parameters bound at a use site
///
/// - status: partial
/// - issues: clangd#230
/// - order: 8
///
/// A use of a template shows the substituted types (`Wrapper<int>`,
/// `identity<int>`, `int x`), but not an explicit `T = int` mapping of each
/// parameter to the argument it was bound to.

namespace instantiation_args {

template <typename T> struct Wrapper {
  T value;
};

template <typename T> T identity(T x) {
  return x;
}

void demo() {
  §(01_type_use)Wrapper<int> holder;
  int r = §(02_call)identity(42);
}

}
