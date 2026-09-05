/// # CTAD
///
/// - status: partial
/// - issues: clangd#435
///
/// Deduced template arguments of a class placeholder
///
/// With class template argument deduction the variable's card shows the
/// deduced `Box<int>`, but hovering the class-name spelling still reports
/// the primary template without its arguments.

namespace ctad_arguments {

template <typename T> struct Box {
  Box(T);
};

§(01_ctad_type)Box §(02_ctad_var)picked(42);

}
